// instance.cpp 

/**
*    Copyright (C) 2008 10gen Inc.
*    Copyright (C) 2013 Tokutek Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include <fstream>
#if !defined(_WIN32)
#include <sys/file.h>
#endif

#include <boost/thread/thread.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/bson/util/atomic_int.h"

#include "mongo/db/databaseholder.h"
#include "mongo/db/introspect.h"
#include "mongo/db/repl.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/instance.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/security.h"
#include "mongo/db/json.h"
#include "mongo/db/replutil.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/background.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/index.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/relock.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/env.h"

#include "mongo/s/d_logic.h"

#include "mongo/util/goodies.h"
#include "mongo/util/time_support.h"

namespace mongo {
    
    // for diaglog
    inline void opread(Message& m) { if( _diaglog.getLevel() & 2 ) _diaglog.readop((char *) m.singleData(), m.header()->len); }
    inline void opwrite(Message& m) { if( _diaglog.getLevel() & 1 ) _diaglog.writeop((char *) m.singleData(), m.header()->len); }

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, CurOp& op);
    void receivedDelete(Message& m, CurOp& op);
    void receivedInsert(Message& m, CurOp& op);
    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop );

    int nloggedsome = 0;
#define LOGWITHRATELIMIT if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    bool useHints = true;

    KillCurrentOp killCurrentOp;

    int lockFile = 0;
#ifdef _WIN32
    HANDLE lockFileHandle;
#endif


    /*static*/ OpTime OpTime::_now() {
        OpTime result;
        unsigned t = (unsigned) time(0);
        if ( last.secs == t ) {
            last.i++;
            result = last;
        }
        else if ( t < last.secs ) {
            result = skewed(); // separate function to keep out of the hot code path
        }
        else { 
            last = OpTime(t, 1);
            result = last;
        }
        notifier.notify_all();
        return last;
    }
    OpTime OpTime::now(const mongo::mutex::scoped_lock&) {
        return _now();
    }
    OpTime OpTime::getLast(const mongo::mutex::scoped_lock&) {
        return last;
    }
    boost::condition OpTime::notifier;
    mongo::mutex OpTime::m("optime");

    // OpTime::now() uses mutex, thus it is in this file not in the cpp files used by drivers and such
    void BSONElementManipulator::initTimestamp() {
        massert( 10332 ,  "Expected CurrentTime type", _element.type() == Timestamp );
        unsigned long long &timestamp = *( reinterpret_cast< unsigned long long* >( value() ) );
        if ( timestamp == 0 ) {
            mutex::scoped_lock lk(OpTime::m);
            timestamp = OpTime::now(lk).asDate();
        }
    }

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObjBuilder b;

        if( ! cc().isAdmin() ) {
            b.append("err", "unauthorized");
        }
        else {
            DbMessage d(m);
            QueryMessage q(d);
            bool all = q.query["$all"].trueValue();
            vector<BSONObj> vals;
            {
                Client& me = cc();
                scoped_lock bl(Client::clientsMutex);
                scoped_ptr<Matcher> m(new Matcher(q.query));
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    verify( c );
                    CurOp* co = c->curop();
                    if ( c == &me && !co ) {
                        continue;
                    }
                    verify( co );
                    if( all || co->displayInCurop() ) {
                        BSONObj info = co->infoNoauth();
                        if ( all || m->matches( info )) {
                            vals.push_back( info );
                        }
                    }
                }
            }
            b.append("inprog", vals);
            if( lockedForWriting() ) {
                b.append("fsyncLock", true);
                b.append("info", "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
            }
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }

    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        if( ! cc().isAdmin() ) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() )
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            DbMessage d(m);
            QueryMessage q(d);
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) {
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else {
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                killCurrentOp.kill( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    bool _unlockFsync();
    void unlockFsync(const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;
        if ( ! cc().isAdmin() ) { // checks auth
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if (strncmp(ns, "admin.", 6) != 0 ) {
            obj = fromjson("{\"err\":\"unauthorized - this command must be run against the admin DB\"}");
        }
        else {
            log() << "command: unlock requested" << endl;
            if( _unlockFsync() ) {
                obj = fromjson("{ok:1,\"info\":\"unlock completed\"}");
            }
            else {
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(Client& c, DbResponse& dbresponse, Message& m ) {
        bool ok = true;
        MSGID responseTo = m.header()->id;

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());

        shared_ptr<AssertionException> ex;

        try {
            dbresponse.exhaustNS = runQuery(m, q, op, *resp);
            verify( !resp->empty() );
        }
        catch ( SendStaleConfigException& e ){
            ex.reset( new SendStaleConfigException( e.getns(), e.getInfo().msg, e.getVersionReceived(), e.getVersionWanted() ) );
            ok = false;
        }
        catch ( AssertionException& e ) {
            ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
            ok = false;
        }

        if( ex ){

            op.debug().exceptionInfo = ex->getInfo();
            LOGWITHRATELIMIT {
                log() << "assertion " << ex->toString() << " ns:" << q.ns << " query:" <<
                (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
                if( q.ntoskip || q.ntoreturn )
                    log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;
            }

            SendStaleConfigException* scex = NULL;
            if ( ex->getCode() == SendStaleConfigCode ) scex = static_cast<SendStaleConfigException*>( ex.get() );

            BSONObjBuilder err;
            ex->getInfo().append( err );
            if( scex ){
                err.append( "ns", scex->getns() );
                scex->getVersionReceived().addToBSON( err, "vReceived" );
                scex->getVersionWanted().addToBSON( err, "vWanted" );
            }
            BSONObj errObj = err.done();

            if( scex ){
                log() << "stale version detected during query over "
                      << q.ns << " : " << errObj << endl;
            }
            else{
                log() << "problem detected during query over "
                      << q.ns << " : " << errObj << endl;
            }

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult * msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->_resultFlags() = ResultFlag_ErrSet;
            if( scex ) qr->_resultFlags() |= ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            resp.reset( new Message() );
            resp->setData( msgdata, true );

        }

        op.debug().responseLength = resp->header()->dataLen();

        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;

        return ok;
    }

    void (*reportEventToSystem)(const char *msg) = 0;

    void mongoAbort(const char *msg) { 
        if( reportEventToSystem ) 
            reportEventToSystem(msg);
        rawOut(msg);
        ::abort();
    }

    // Returns false when request includes 'end'
    void assembleResponse( Message &m, DbResponse &dbresponse, const HostAndPort& remote ) {

        // before we lock...
        int op = m.operation();
        bool isCommand = false;
        const char *ns = m.singleData()->_data + 4;

        if ( op == dbQuery ) {
            if( strstr(ns, ".$cmd") ) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) {
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) {
                        killOp(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) {
                        unlockFsync(ns, m, dbresponse);
                        return;
                    }
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
        }
        else {
            opwrite(m);
        }

        globalOpCounters.gotOp( op , isCommand );

        Client& c = cc();
        if ( c.getAuthenticationInfo() ) {
            c.getAuthenticationInfo()->startRequest();
        }

        // initialize the default OpSettings, 
        OpSettings settings;
        c.setOpSettings(settings);
        
        auto_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ) {
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }
        else {
            c.newTopLevelRequest();
        }

        CurOp& currentOp = *currentOpP;
        currentOp.reset(remote,op);

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        long long logThreshold = cmdLine.slowMS;
        bool shouldLog = logLevel >= 1;

        if ( op == dbQuery ) {
            if ( handlePossibleShardedMessage( m , &dbresponse ) )
                return;
            receivedQuery(c , dbresponse, m );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(dbresponse, m, currentOp) )
                shouldLog = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            char *p = m.singleData()->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis64() % 10000 <<
                      " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
        }
        else {
            try {
                const NamespaceString nsString( ns );

                // The following operations all require authorization.
                // dbInsert, dbUpdate and dbDelete can be easily pre-authorized,
                // here, but dbKillCursors cannot.
                if ( op == dbKillCursors ) {
                    currentOp.ensureStarted();
                    logThreshold = 10;
                    receivedKillCursors(m);
                }
                else if ( !nsString.isValid() ) {
                    // Only killCursors doesn't care about namespaces
                    uassert( 16257, str::stream() << "Invalid ns [" << ns << "]", false );
                }
                else if ( ! c.getAuthenticationInfo()->isAuthorized(
                                  nsToDatabase( m.singleData()->_data + 4 ) ) ) {
                    setLastError(0, "unauthorized");
                }
                else if ( op == dbInsert ) {
                    receivedInsert(m, currentOp);
                }
                else if ( op == dbUpdate ) {
                    receivedUpdate(m, currentOp);
                }
                else if ( op == dbDelete ) {
                    receivedDelete(m, currentOp);
                }
                else {
                    mongo::log() << "    operation isn't supported: " << op << endl;
                    currentOp.done();
                    shouldLog = true;
                }
            }
            catch ( UserException& ue ) {
                tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                        << ue.toString() << endl;
                debug.exceptionInfo = ue.getInfo();
            }
            catch ( AssertionException& e ) {
                tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                        << e.toString() << endl;
                debug.exceptionInfo = e.getInfo();
                shouldLog = true;
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        debug.executionTime = currentOp.totalTimeMillis();

        logThreshold += currentOp.getExpectedLatencyMs();

        if ( (shouldLog || debug.executionTime > logThreshold) && !debug.vetoLog(currentOp) ) {
            mongo::tlog() << debug.report( currentOp ) << endl;
        }

        if ( currentOp.shouldDBProfile( debug.executionTime ) ) {
            // performance profiling is on
            if ( Lock::isReadLocked() ) {
                LOG(1) << "note: not profiling because recursive read lock" << endl;
            }
            else if ( lockedForWriting() ) {
                LOG(1) << "note: not profiling because doing fsync+lock" << endl;
            }
            else {
                Lock::DBWrite lk( currentOp.getNS() );
                if ( dbHolder()._isLoaded( nsToDatabase( currentOp.getNS() ) , dbpath ) ) {
                    Client::Context cx( currentOp.getNS(), dbpath, false );
                    Client::AlternateTransactionStack altStack;
                    Client::Transaction txn(DB_SERIALIZABLE);
                    profile(c, op, currentOp);
                    txn.commit();
                }
                else {
                    mongo::log() << "note: not profiling because db went away - probably a close on: " << currentOp.getNS() << endl;
                }
            }
        }
        
        debug.reset();
    } /* assembleResponse() */

    void receivedKillCursors(Message& m) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        uassert( 13659 , "sent 0 cursors to kill" , n != 0 );
        massert( 13658 , str::stream() << "bad kill cursors size: " << m.dataSize() , m.dataSize() == 8 + ( 8 * n ) );
        uassert( 13004 , str::stream() << "sent negative cursors to kill: " << n  , n >= 1 );

        if ( n > 2000 ) {
            LOG( n < 30000 ? LL_WARNING : LL_ERROR ) << "receivedKillCursors, n=" << n << endl;
            verify( n < 30000 );
        }

        int found = ClientCursor::erase(n, (long long *) x);

        if ( logLevel > 0 || found != n ) {
            LOG( found == n ? 1 : 0 ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    /* db - database name
       path - db directory
    */
    void Database::closeDatabase( const char *name, const string& path ) {
        verify( Lock::isW() );

        Client::Context * ctx = cc().getContext();
        verify( ctx );
        verify( ctx->inDB( name , path ) );
        Database *database = ctx->db();
        verify( database->name() == name );

        /* important: kill all open cursors on the database */
        string prefix(name);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::clearForPrefix( prefix.c_str() );

        dbHolderW().erase( name, path );
        ctx->_clear();
        delete database; // closes files
    }

    static void lockedReceivedUpdate(const char *ns, Message &m, CurOp &op, const BSONObj &toupdate, const BSONObj &query, int flags) {
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;

        // void ReplSetImpl::relinquish() uses big write lock so 
        // this is thus synchronized given our lock above.
        uassert(10054,  "not master", isMasterNs(ns));

        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if (!broadcast && handlePossibleShardedMessage(m, 0)) {
            return;
        }

        Client::Context ctx(ns);
        Client::Transaction transaction(DB_SERIALIZABLE);
        UpdateResult res = updateObjects(ns, toupdate, query, upsert, multi, true, op.debug() );
        lastError.getSafe()->recordUpdate( res.existing , res.num , res.upserted ); // for getlasterror
        transaction.commit();
    }

    void receivedUpdate(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        verify(d.moreJSObjs());
        verify(query.objsize() < m.header()->dataLen());
        BSONObj toupdate = d.nextJsObj();
        uassert(10055, "update object too large", toupdate.objsize() <= BSONObjMaxUserSize);
        verify(toupdate.objsize() < m.header()->dataLen());
        verify(query.objsize() + toupdate.objsize() < m.header()->dataLen());

        op.debug().query = query;
        op.setQuery(query);

        OpSettings settings;
        settings.setQueryCursorMode(WRITE_LOCK_CURSOR);
        cc().setOpSettings(settings);

        try {
            Lock::DBRead lk(ns);
            lockedReceivedUpdate(ns, m, op, toupdate, query, flags);
        }
        catch (RetryWithWriteLock &e) {
            Lock::DBWrite lk(ns);
            lockedReceivedUpdate(ns, m, op, toupdate, query, flags);
        }
    }

    void receivedDelete(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        verify(d.moreJSObjs());
        BSONObj pattern = d.nextJsObj();

        op.debug().query = pattern;
        op.setQuery(pattern);

        OpSettings settings;
        settings.setQueryCursorMode(WRITE_LOCK_CURSOR);
        cc().setOpSettings(settings);

        bool justOne = flags & RemoveOption_JustOne;
        bool broadcast = flags & RemoveOption_Broadcast;

        Lock::DBRead lk(ns);

        // writelock is used to synchronize stepdowns w/ writes
        uassert(10056, "not master", isMasterNs(ns));

        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if (!broadcast && handlePossibleShardedMessage(m, 0)) {
            return;
        }

        Client::Context ctx(ns);
        Client::Transaction transaction(DB_SERIALIZABLE);
        long long n = deleteObjects(ns, pattern, justOne, true);
        lastError.getSafe()->recordDelete( n );
        transaction.commit();
    }

    QueryResult* emptyMoreResult(long long);

    void OpTime::waitForDifferent(unsigned millis){
        mutex::scoped_lock lk(m);
        while (*this == last) {
            if (!notifier.timed_wait(lk.boost(), boost::posix_time::milliseconds(millis)))
                return; // timed out
        }
    }

    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop ) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        shared_ptr<AssertionException> ex;
        scoped_ptr<Timer> timer;
        int pass = 0;
        bool exhaust = false;
        QueryResult* msgdata = 0;
        GTID last;
        bool isOplog = false;
        while( 1 ) {
            try {
                const NamespaceString nsString( ns );
                uassert( 16258, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

                // I (Zardosht), am not crazy about this, but I cannot think of
                // better alternatives at the moment. The high level goal is to find
                // a way to do a wait without having a read lock held 
                // via Client::ReadContext. Unfortunately, we can't get the exact position
                // of the cursor without accessing it, which required a read lock.
                // So, we do this, which is a good estimate.
                //
                // Note this is similar to what vanilla MongoDB does.
                //
                // in the first pass, we extract the minimum live GTID. This must be
                // greater than or equal to the existing cursor's position. 
                // In the second pass, we wait for the GTID manager to have a 
                // minumum live GTID greater than what we saw in the first pass.
                // This new GTID will be greater than the cursor's starting position,
                // and therefore the cursor should have more data to look at.
                // It is theoretically possible that one day, the cursor will still
                // return no new data because all new GTIDs in between these
                // two values aborted, but that is not possible right now. Any GTID
                // assigned is done so with the intent to commit, and tokudb
                // aborts if a coommit is not successful.
                if (str::startsWith(ns, "local.oplog.") && theReplSet){
                    isOplog = true;
                    if (pass == 0) {
                        last = theReplSet->gtidManager->getMinLiveGTID();
                    }
                    else {
                        theReplSet->gtidManager->waitForDifferentMinLive(
                            last, 
                            4000 // ms
                            );
                    }
                }

                Client::ReadContext ctx(ns);

                // call this readlocked so state can't change
                replVerifyReadsOk();
                msgdata = processGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust);
            }
            catch ( AssertionException& e ) {
                ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
                ok = false;
                break;
            }
            
            pass++;
            if (msgdata == 0) {
                // this should only happen with QueryOption_AwaitData
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
                if (!isOplog) {
                    if ( ! timer ) {
                        timer.reset( new Timer() );
                    }
                    else {
                        if ( timer->seconds() >= 4 ) {
                            // after about 4 seconds, return. pass stops at 1000 normally.
                            // we want to return occasionally so slave can checkpoint.
                            pass = 10000;
                        }
                    }
                    if (debug) {
                        sleepmillis(20);
                    }
                    else {
                        sleepmillis(2);
                    }
                }
                else {
                    // in the case where we are the oplog, using 
                    // waitForDifferentMinLive is sufficient. That 
                    // waits for 4 seconds, as the timer above does.
                    // So, we we don't need more than 2 passes.
                    if (pass > 1) {
                        pass = 10000;
                    }
                }

                // TODO: (Zardosht), figure out what this is meant to do
                
                // note: the 1100 is beacuse of the waitForDifferent above
                // should eventually clean this up a bit
                //curop.setExpectedLatencyMs( 1100 + timer->millis() );
                
                continue;
            }
            break;
        };

        if (ex) {
            exhaust = false;

            BSONObjBuilder err;
            ex->getInfo().append( err );
            BSONObj errObj = err.done();

            if (!ex->interrupted()) {
                log() << errObj << endl;
            }

            curop.debug().exceptionInfo = ex->getInfo();

            if (ex->getCode() == 13436) {
                replyToQuery(ResultFlag_ErrSet, m, dbresponse, errObj);
                curop.debug().responseLength = dbresponse.response->header()->dataLen();
                curop.debug().nreturned = 1;
                return ok;
            }

            msgdata = emptyMoreResult(cursorid);
        }

        Message *resp = new Message();
        resp->setData(msgdata, true);
        curop.debug().responseLength = resp->header()->dataLen();
        curop.debug().nreturned = msgdata->nReturned;

        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
        
        if( exhaust ) {
            curop.debug().exhaust = true;
            dbresponse.exhaustNS = ns;
        }

        return ok;
    }

    static void lockedReceivedInsert(const char *ns, Message &m, const vector<BSONObj> &objs, bool keepGoing) {
        // writelock is used to synchronize stepdowns w/ writes
        uassert(10058, "not master", isMasterNs(ns));

        if (handlePossibleShardedMessage(m, 0)) {
            return;
        }

        Client::Context ctx(ns);
        Client::Transaction transaction(DB_SERIALIZABLE);
        insertObjects(ns, objs, keepGoing, 0, true);
        globalOpCounters.incInsertInWriteLock(objs.size());
        transaction.commit();
    }

    void receivedInsert(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;

        if (!d.moreJSObjs()) {
            // strange.  should we complain?
            return;
        }

        vector<BSONObj> objs;
        while (d.moreJSObjs()) {
            objs.push_back(d.nextJsObj());
        }

        const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;

        OpSettings settings;
        settings.setQueryCursorMode(WRITE_LOCK_CURSOR);
        cc().setOpSettings(settings);

        try {
            Lock::DBRead lk(ns);
            lockedReceivedInsert(ns, m, objs, keepGoing);
        }
        catch (RetryWithWriteLock &e) {
            Lock::DBWrite lk(ns);
            lockedReceivedInsert(ns, m, objs, keepGoing);
        }
    }

    struct getDatabaseNamesExtra {
        vector<string> &names;
        getDatabaseNamesExtra(vector<string> &n) : names(n) {}
    };

    static int getDatabaseNamesCallback(const DBT *key, const DBT *val, void *extra) {
        getDatabaseNamesExtra *e = static_cast<getDatabaseNamesExtra *>(extra);
        size_t length = key->size;
        if (length > 0) {
            // strip off the trailing 0 in the key
            char *cp = (char *) key->data + length - 1;
            if (*cp == 0)
                length -= 1;
            if (length >= 3 && strcmp((char *) key->data + length - 3, ".ns") == 0) {
                e->names.push_back(string((char *) key->data, length - 3));
            }
        }
        return 0;
    } 

    void getDatabaseNames( vector< string > &names) {
        verify(Lock::isRW());
        // create a cursor on the tokudb directory and search for <database>.ns keys
        storage::DirectoryCursor c(storage::env, cc().txn().db_txn());
        getDatabaseNamesExtra extra(names);
        int r = 0;
        while (r == 0) {
            r = c.dbc()->c_getf_next(c.dbc(), 0, getDatabaseNamesCallback, &extra);
            if (r != 0 && r != DB_NOTFOUND)
                storage::handle_ydb_error(r);
        }
    }

    /* returns true if there is data on this server.  useful when starting replication.
       local database does NOT count except for rsoplog collection.
       used to set the hasData field on replset heartbeat command response
    */
    bool replHasDatabases() {
        vector<string> names;
        {
            Lock::GlobalRead lk;
            Client::Transaction txn(DB_TXN_READ_ONLY | DB_TXN_SNAPSHOT);
            getDatabaseNames(names);
            txn.commit();
        }
        if( names.size() >= 2 ) return true;
        if( names.size() == 1 ) {
            if( names[0] != "local" )
                return true;
            // we have a local database.  return true if oplog isn't empty
            {
                Lock::DBRead lk(rsoplog);
                BSONObj o;
                if( Helpers::getFirst(rsoplog, o) )
                    return true;
            }
        }
        return false;
    }

    QueryOptions DBDirectClient::_lookupAvailableOptions() {
        // Exhaust mode is not available in DBDirectClient.
        return QueryOptions(DBClientBase::_lookupAvailableOptions() & ~QueryOption_Exhaust);
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
        verify( dbResponse.response );
        dbResponse.response->concat(); // can get rid of this if we make response handling smarter
        response = *dbResponse.response;
        return true;
    }

    void DBDirectClient::say( Message &toSend, bool isRetry, string * actualServer ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string &ns, Query query, int nToReturn , int nToSkip ,
            const BSONObj *fieldsToReturn , int queryOptions , int batchSize) {

        //if ( ! query.obj.isEmpty() || nToReturn != 0 || nToSkip != 0 || fieldsToReturn || queryOptions )
        return DBClientBase::query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
        //
        //verify( query.obj.isEmpty() );
        //throw UserException( (string)"yay:" + ns );
    }

    void DBDirectClient::killCursor( long long id ) {
        ClientCursor::erase( id );
    }

    HostAndPort DBDirectClient::_clientHost = HostAndPort( "0.0.0.0" , 0 );

    unsigned long long DBDirectClient::count(const string &ns, const BSONObj& query, int options, int limit, int skip ) {
        string errmsg;
        int errCode;
        long long res = runCount( ns.c_str() , _countCmd( ns , query , options , limit , skip ) , errmsg, errCode );
        if ( res == -1 ) {
            // namespace doesn't exist
            return 0;
        }
        massert( errCode , str::stream() << "count failed in DBDirectClient: " << errmsg , res >= 0 );
        return (unsigned long long )res;
    }

    DBClientBase * createDirectClient() {
        return new DBDirectClient();
    }

    mongo::mutex exitMutex("exit");
    AtomicUInt numExitCalls = 0;

    bool inShutdown() {
        return numExitCalls > 0;
    }

    void tryToOutputFatal( const string& s ) {
        try {
            rawOut( s );
            return;
        }
        catch ( ... ) {}

        try {
            cerr << s << endl;
            return;
        }
        catch ( ... ) {}

        // uh - oh, not sure there is anything else we can do...
    }

    static void shutdownServer() {

        log() << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log() << "shutdown: going to flush diaglog..." << endl;
        _diaglog.flush();

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( boost::bind(MessagingPort::closeAllSockets, 0) );

        {
            Lock::GlobalWrite lk;
            log() << "shutdown: going to close databases..." << endl;
            dbHolderW().closeDatabases(dbpath);
            log() << "shutdown: going to shutdown TokuKV..." << endl;
            storage::shutdown();
        }

#if !defined(__sunos__)
        if ( lockFile ) {
            log() << "shutdown: removing fs lock..." << endl;
            /* This ought to be an unlink(), but Eliot says the last
               time that was attempted, there was a race condition
               with acquirePathLock().  */
#ifdef _WIN32
            if( _chsize( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << WSAGetLastError() << endl;
            CloseHandle(lockFileHandle);
#else
            if( ftruncate( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
#endif
        }
#endif
    }

    void exitCleanly( ExitCode code ) {
        killCurrentOp.killAll();
        if (theReplSet) {
            theReplSet->shutdown();
        }


        {
            Lock::GlobalWrite lk;
            log() << "now exiting" << endl;
            dbexit( code );
        }
    }

    /* not using log() herein in case we are already locked */
    NOINLINE_DECL void dbexit( ExitCode rc, const char *why ) {

        Client * c = currentClient.get();
        {
            scoped_lock lk( exitMutex );
            if ( numExitCalls++ > 0 ) {
                if ( numExitCalls > 5 ) {
                    // this means something horrible has happened
                    ::_exit( rc );
                }
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately";
                tryToOutputFatal( ss.str() );
                if ( c ) c->shutdown();
                ::_exit( rc );
            }
        }

        {
            stringstream ss;
            ss << "dbexit: " << why;
            tryToOutputFatal( ss.str() );
        }

        try {
            shutdownServer(); // gracefully shutdown instance
        }
        catch ( ... ) {
            tryToOutputFatal( "shutdown failed with exception" );
        }

#if defined(_DEBUG)
        try {
            mutexDebugger.programEnding();
        }
        catch (...) { }
#endif

        // block the dur thread from doing any work for the rest of the run
        LOG(2) << "shutdown: groupCommitMutex" << endl;
        //SimpleMutex::scoped_lock lk(dur::commitJob.groupCommitMutex);

#ifdef _WIN32
        // Windows Service Controller wants to be told when we are down,
        //  so don't call ::_exit() yet, or say "really exiting now"
        //
        if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
            if ( c ) c->shutdown();
            return;
        }
#endif
        tryToOutputFatal( "dbexit: really exiting now" );
        if ( c ) c->shutdown();
        ::_exit(rc);
    }

#if !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << getpid() << endl;
        string s = ss.str();
        const char * data = s.c_str();
#ifdef _WIN32
        verify( _write( fd, data, strlen( data ) ) );
#else
        verify( write( fd, data, strlen( data ) ) );
#endif
    }

    void acquirePathLock() {
        string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();

#ifdef _WIN32
        lockFileHandle = CreateFileA( name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0 /* do not allow anyone else access */, NULL, 
            OPEN_ALWAYS /* success if fh can open */, 0, NULL );

        if (lockFileHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            char *msg;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&msg, 0, NULL);
            string m = msg;
            str::stripTrailing(m, "\r\n");
            uasserted( 13627 , str::stream() << "Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?" );
        }
        lockFile = _open_osfhandle((intptr_t)lockFileHandle, 0);
#else
        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
        if( lockFile <= 0 ) {
            uasserted( 10309 , str::stream() << "Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?" );
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uassert( 10310 ,  "Unable to lock file: " + name + ". Is a mongod instance already running?",  0 );
        }
#endif

#ifdef _WIN32
        uassert( 13625, "Unable to truncate lock file", _chsize(lockFile, 0) == 0);
        writePid( lockFile );
        _commit( lockFile );
#else
        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
        flushMyDirectory(name);
#endif
    }
#else
#endif

    // ----- BEGIN Diaglog -----
    DiagLog::DiagLog() : f(0) , level(0), mutex("DiagLog") { 
    }

    void DiagLog::openFile() {
        verify( f == 0 );
        stringstream ss;
        ss << dbpath << "/diaglog." << hex << time(0);
        string name = ss.str();
        f = new ofstream(name.c_str(), ios::out | ios::binary);
        if ( ! f->good() ) {
            problem() << "diagLogging couldn't open " << name << endl;
            // todo what is this? :
            throw 1717;
        }
        else {
            log() << "diagLogging using file " << name << endl;
        }
    }

    int DiagLog::setLevel( int newLevel ) {
        scoped_lock lk(mutex);
        int old = level;
        log() << "diagLogging level=" << newLevel << endl;
        if( f == 0 ) { 
            openFile();
        }
        level = newLevel; // must be done AFTER f is set
        return old;
    }
    
    void DiagLog::flush() {
        if ( level ) {
            log() << "flushing diag log" << endl;
            scoped_lock lk(mutex);
            f->flush();
        }
    }
    
    void DiagLog::writeop(char *data,int len) {
        if ( level & 1 ) {
            scoped_lock lk(mutex);
            f->write(data,len);
        }
    }
    
    void DiagLog::readop(char *data, int len) {
        if ( level & 2 ) {
            bool log = (level & 4) == 0;
            OCCASIONALLY log = true;
            if ( log ) {
                scoped_lock lk(mutex);
                verify( f );
                f->write(data,len);
            }
        }
    }

    DiagLog _diaglog;

    // ----- END Diaglog -----

} // namespace mongo
