// dbclient.cpp - connect to a Mongo database as a database, from C++

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "stdafx.h"
#include "../db/pdfile.h"
#include "dbclient.h"
#include "../util/builder.h"
#include "../db/jsobj.h"
#include "../db/query.h"
#include "../db/json.h"
#include "../db/instance.h"
#include "../util/md5.hpp"

namespace mongo {

    Query& Query::where(const char *jscode, BSONObj scope) { 
        /* use where() before sort() and hint() and explain(), else this will assert. */
        assert( !obj.hasField("query") );
        BSONObjBuilder b;
        b.appendElements(obj);
        b.appendWhere(jscode, scope);
        obj = b.obj();
        return *this;
    }

    Query& Query::sort(const BSONObj& s) { 
        BSONObjBuilder b;
        if( obj.hasElement("query") )
            b.appendElements(obj);
        else
            b.append("query", obj);
        b.append("orderby", s);
        obj = b.obj();
        return *this; 
    }

    Query& Query::hint(BSONObj keyPattern) {
        BSONObjBuilder b;
        if( obj.hasElement("query") )
            b.appendElements(obj);
        else
            b.append("query", obj);
        b.append("$hint", keyPattern);
        obj = b.obj();
        return *this; 
    }

    Query& Query::explain() {
        BSONObjBuilder b;
        if( obj.hasElement("query") )
            b.appendElements(obj);
        else
            b.append("query", obj);
        b.append("$explain", true);
        obj = b.obj();
        return *this; 
    }
    
    bool Query::isComplex() const{
        return obj.hasElement( "query" );
    }
        
    BSONObj Query::getFilter() const {
        if ( ! isComplex() )
            return obj;
        return obj.getObjectField( "query" );
    }
    BSONObj Query::getSort() const {
        if ( ! isComplex() )
            return emptyObj;
        return obj.getObjectField( "orderby" );
    }
    BSONObj Query::getHint() const {
        if ( ! isComplex() )
            return emptyObj;
        return obj.getObjectField( "$hint" );
    }
    bool Query::isExplain() const {
        return isComplex() && obj.getBoolField( "$explain" );
    }
    
    string Query::toString() const{
        return obj.toString();
    }

    /* --- dbclientcommands --- */

    inline bool DBClientWithCommands::isOk(const BSONObj& o) {
        return o.getIntField("ok") == 1;
    }

    inline bool DBClientWithCommands::runCommand(const char *dbname, const BSONObj& cmd, BSONObj &info) {
        string ns = string(dbname) + ".$cmd";
        info = findOne(ns.c_str(), cmd);
        return isOk(info);
    }

    /* note - we build a bson obj here -- for something that is super common like getlasterror you
              should have that object prebuilt as that would be faster.
    */
    bool DBClientWithCommands::simpleCommand(const char *dbname, BSONObj *info, const char *command) {
        BSONObj o;
        if ( info == 0 )
            info = &o;
        BSONObjBuilder b;
        b.append(command, 1);
        return runCommand(dbname, b.done(), *info);
    }

    unsigned long long DBClientWithCommands::count(const char *_ns, BSONObj query) { 
        NamespaceString ns(_ns);
        BSONObj cmd = BSON( "count" << ns.coll << "query" << query );
        BSONObj res;
        if( !runCommand(ns.db.c_str(), cmd, res) )
            uasserted(string("count fails:") + res.toString());
        return res.getIntField("n");
    }

    BSONObj getlasterrorcmdobj = fromjson("{getlasterror:1}");

    string DBClientWithCommands::getLastError() { 
        BSONObj info;
        runCommand("admin", getlasterrorcmdobj, info);
        BSONElement e = info["err"];
        if( e.eoo() ) return "";
        if( e.type() == Object ) return e.toString();
        return e.str();
    }

    BSONObj getpreverrorcmdobj = fromjson("{getpreverror:1}");

    BSONObj DBClientWithCommands::getPrevError() { 
        BSONObj info;
        runCommand("admin", getpreverrorcmdobj, info);
        return info;
    }

    BSONObj getnoncecmdobj = fromjson("{getnonce:1}");

    string DBClientWithCommands::createPasswordDigest( const char * username , const char * clearTextPassword ){
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) username, strlen(username));
            md5_append(&st, (const md5_byte_t *) ":mongo:", 7 );
            md5_append(&st, (const md5_byte_t *) clearTextPassword, strlen(clearTextPassword));
            md5_finish(&st, d);
        }
        return digestToString( d );
    }

    bool DBClientWithCommands::auth(const char *dbname, const char *username, const char *password_text, string& errmsg, bool digestPassword) {
		//cout << "TEMP AUTH " << toString() << dbname << ' ' << username << ' ' << password_text << ' ' << digestPassword << endl;

		string password = password_text;
		if( digestPassword ) 
			password = createPasswordDigest( username , password_text );

        BSONObj info;
        string nonce;
        if( !runCommand(dbname, getnoncecmdobj, info) ) {
            errmsg = "getnonce fails - connection problem?";
            return false;
        }
        {
            BSONElement e = info.getField("nonce");
            assert( e.type() == String );
            nonce = e.valuestr();
        }

        BSONObj authCmd;
        BSONObjBuilder b;
        {

            b << "authenticate" << 1 << "nonce" << nonce << "user" << username;
            md5digest d;
            {
                md5_state_t st;
                md5_init(&st);
                md5_append(&st, (const md5_byte_t *) nonce.c_str(), nonce.size() );
                md5_append(&st, (const md5_byte_t *) username, strlen(username));
                md5_append(&st, (const md5_byte_t *) password.c_str(), password.size() );
                md5_finish(&st, d);
            }
            b << "key" << digestToString( d );
            authCmd = b.done();
        }
        
        if( runCommand(dbname, authCmd, info) ) 
            return true;

        errmsg = info.toString();
        return false;
    }

    BSONObj ismastercmdobj = fromjson("{\"ismaster\":1}");

    bool DBClientWithCommands::isMaster(bool& isMaster, BSONObj *info) {
        BSONObj o;
        if ( info == 0 )	info = &o;
        bool ok = runCommand("admin", ismastercmdobj, *info);
        isMaster = (info->getIntField("ismaster") == 1);
        return ok;
    }

    bool DBClientWithCommands::createCollection(const char *ns, unsigned size, bool capped, int max, BSONObj *info) {
        BSONObj o;
        if ( info == 0 )	info = &o;
        BSONObjBuilder b;
        b.append("create", ns);
        if ( size ) b.append("size", size);
        if ( capped ) b.append("capped", true);
        if ( max ) b.append("max", max);
        string db = nsToClient(ns);
        return runCommand(db.c_str(), b.done(), *info);
    }

    bool DBClientWithCommands::copyDatabase(const char *fromdb, const char *todb, const char *fromhost, BSONObj *info) {
        assert( *fromdb && *todb );
        BSONObj o;
        if ( info == 0 ) info = &o;
        BSONObjBuilder b;
        b.append("copydb", 1);
        b.append("fromhost", fromhost);
        b.append("fromdb", fromdb);
        b.append("todb", todb);
        return runCommand("admin", b.done(), *info);
    }

    bool DBClientWithCommands::setDbProfilingLevel(const char *dbname, ProfilingLevel level, BSONObj *info ) {
        BSONObj o;
        if ( info == 0 ) info = &o;

        if ( level ) {
            // Create system.profile collection.  If it already exists this does nothing.
            // TODO: move this into the db instead of here so that all
            //       drivers don't have to do this.
            string ns = string(dbname) + ".system.profile";
            createCollection(ns.c_str(), 1024 * 1024, true, 0, info);
        }

        BSONObjBuilder b;
        b.append("profile", (int) level);
        return runCommand(dbname, b.done(), *info);
    }

    BSONObj getprofilingcmdobj = fromjson("{\"profile\":-1}");

    bool DBClientWithCommands::getDbProfilingLevel(const char *dbname, ProfilingLevel& level, BSONObj *info) {
        BSONObj o;
        if ( info == 0 ) info = &o;
        if ( runCommand(dbname, getprofilingcmdobj, *info) ) {
            level = (ProfilingLevel) info->getIntField("was");
            return true;
        }
        return false;
    }

    bool DBClientWithCommands::eval(const char *dbname, const char *jscode, BSONObj& info, BSONElement& retValue, BSONObj *args) {
        BSONObjBuilder b;
        b.appendCode("$eval", jscode);
        if ( args )
            b.appendArray("args", *args);
        bool ok = runCommand(dbname, b.done(), info);
        if ( ok )
            retValue = info.getField("retval");
        return ok;
    }

    bool DBClientWithCommands::eval(const char *dbname, const char *jscode) {
        BSONObj info;
        BSONElement retValue;
        return eval(dbname, jscode, info, retValue);
    }

    void testSort() { 
        DBClientConnection c;
        string err;
        if ( !c.connect("localhost", err) ) {
            out() << "can't connect to server " << err << endl;
            return;
        }

        cout << "findOne returns:" << endl;
        cout << c.findOne("test.foo", QUERY( "x" << 3 ) ).toString() << endl;
        cout << c.findOne("test.foo", QUERY( "x" << 3 ).sort("name") ).toString() << endl;

    }

    /* TODO: unit tests should run this? */
    void testDbEval() {
        DBClientConnection c;
        string err;
        if ( !c.connect("localhost", err) ) {
            out() << "can't connect to server " << err << endl;
            return;
        }

        if( !c.auth("dwight", "u", "p", err) ) { 
            out() << "can't authenticate " << err << endl;
            return;
        }

        BSONObj info;
        BSONElement retValue;
        BSONObjBuilder b;
        b.append("0", 99);
        BSONObj args = b.done();
        bool ok = c.eval("dwight", "function() { return args[0]; }", info, retValue, &args);
        out() << "eval ok=" << ok << endl;
        out() << "retvalue=" << retValue.toString() << endl;
        out() << "info=" << info.toString() << endl;

        out() << endl;

        int x = 3;
        assert( c.eval("dwight", "function() { return 3; }", x) );

        out() << "***\n";

        BSONObj foo = fromjson("{\"x\":7}");
        out() << foo.toString() << endl;
        int res=0;
        ok = c.eval("dwight", "function(parm1) { return parm1.x; }", foo, res);
        out() << ok << " retval:" << res << endl;
    }

	void testPaired();
    int test2() {
        testSort();
        return 0;
    }

    /* --- dbclientconnection --- */

	bool DBClientConnection::auth(const char *dbname, const char *username, const char *password_text, string& errmsg, bool digestPassword) {
		string password = password_text;
		if( digestPassword ) 
			password = createPasswordDigest( username , password_text );

		if( autoReconnect ) {
			/* note we remember the auth info before we attempt to auth -- if the connection is broken, we will 
			   then have it for the next autoreconnect attempt. 
			*/
			pair<string,string> p = pair<string,string>(username, password);
			authCache[dbname] = p;
		}

		return DBClientBase::auth(dbname, username, password.c_str(), errmsg, false);
	}

    BSONObj DBClientBase::findOne(const char *ns, Query query, BSONObj *fieldsToReturn, int queryOptions) {
        auto_ptr<DBClientCursor> c =
            this->query(ns, query, 1, 0, fieldsToReturn, queryOptions);

        massert( "DBClientBase::findOne: transport error", c.get() );

        if ( !c->more() )
            return BSONObj();

        return c->next().copy();
    }

    bool DBClientConnection::connect(const char *_serverAddress, string& errmsg) {
        serverAddress = _serverAddress;

        string ip;
        int port;
        size_t idx = serverAddress.find( ":" );
        if ( idx != string::npos ) {
            port = strtol( serverAddress.substr( idx + 1 ).c_str(), 0, 10 );
            ip = serverAddress.substr( 0 , idx );
            ip = hostbyname(ip.c_str());
        } else {
            port = DBPort;
            ip = hostbyname( serverAddress.c_str() );
        }
        massert( "Unable to parse hostname", !ip.empty() );

        // we keep around SockAddr for connection life -- maybe MessagingPort
        // requires that?
        server = auto_ptr<SockAddr>(new SockAddr(ip.c_str(), port));
        p = auto_ptr<MessagingPort>(new MessagingPort());

        if ( !p->connect(*server) ) {
            stringstream ss;
            ss << "couldn't connect to server " << serverAddress << " " << ip << ":" << port;
            errmsg = ss.str();
            failed = true;
            return false;
        }
        return true;
    }

    void DBClientConnection::_checkConnection() {
        if ( !failed )
            return;
        if ( lastReconnectTry && time(0)-lastReconnectTry < 2 )
            return;
        if ( !autoReconnect )
            return;

        lastReconnectTry = time(0);
        log() << "trying reconnect to " << serverAddress << endl;
        string errmsg;
        string tmp = serverAddress;
        failed = false;
        if ( !connect(tmp.c_str(), errmsg) ) { 
            log() << "reconnect " << serverAddress << " failed " << errmsg << endl;
			return;
		}

		log() << "reconnect " << serverAddress << " ok" << endl;
		for( map< string, pair<string,string> >::iterator i = authCache.begin(); i != authCache.end(); i++ ) { 
			const char *dbname = i->first.c_str();
			const char *username = i->second.first.c_str();
			const char *password = i->second.second.c_str();
			if( !DBClientBase::auth(dbname, username, password, errmsg, false) )
				log() << "reconnect: auth failed db:" << dbname << " user:" << username << ' ' << errmsg << '\n';
		}
    }

    auto_ptr<DBClientCursor> DBClientBase::query(const char *ns, Query query, int nToReturn,
            int nToSkip, BSONObj *fieldsToReturn, int queryOptions) {
        auto_ptr<DBClientCursor> c( new DBClientCursor( this,
                                    ns, query.obj, nToReturn, nToSkip,
                                    fieldsToReturn, queryOptions ) );
        if ( c->init() )
            return c;
        return auto_ptr< DBClientCursor >( 0 );
    }

    void DBClientBase::insert( const char * ns , BSONObj obj ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );
        obj.appendSelfToBufBuilder( b );

        toSend.setData( dbInsert , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::insert( const char * ns , const vector< BSONObj > &v ) {
        Message toSend;
        
        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );
        for( vector< BSONObj >::const_iterator i = v.begin(); i != v.end(); ++i )
            i->appendSelfToBufBuilder( b );
        
        toSend.setData( dbInsert, b.buf(), b.len() );
        
        say( toSend );
    }

    void DBClientBase::remove( const char * ns , Query obj , bool justOne ) {
        Message toSend;

        BufBuilder b;
        int opts = 0;
        b.append( opts );
        b.append( ns );

        int flags = 0;
        if ( justOne || obj.obj.hasField( "_id" ) )
            flags |= 1;
        b.append( flags );

        obj.obj.appendSelfToBufBuilder( b );

        toSend.setData( dbDelete , b.buf() , b.len() );

        say( toSend );
    }

    void DBClientBase::update( const char * ns , Query query , BSONObj obj , bool upsert ) {

        BufBuilder b;
        b.append( (int)0 ); // reserverd
        b.append( ns );

        b.append( (int)upsert );

        query.obj.appendSelfToBufBuilder( b );
        obj.appendSelfToBufBuilder( b );

        Message toSend;
        toSend.setData( dbUpdate , b.buf() , b.len() );

        say( toSend );
    }

    bool DBClientBase::ensureIndex( const string &ns , BSONObj keys , const char * name ) {
        BSONObjBuilder toSave;
        toSave.append( "ns" , ns );
        toSave.append( "key" , keys );

        string cacheKey(ns);
        cacheKey += "--";

        if ( name ) {
            toSave.append( "name" , name );
            cacheKey += name;
        }
        else {
            stringstream ss;
            
            bool first = 1;
            for ( BSONObjIterator i(keys); i.more(); ) {
                BSONElement f = i.next();
                if ( f.eoo() )
                    break;

                if ( first )
                    first = 0;
                else
                    ss << "_";

                ss << f.fieldName() << "_";

                if ( f.type() == NumberInt )
                    ss << (int)(f.number() );
                else if ( f.type() == NumberDouble )
                    ss << f.number();

            }

            toSave.append( "name" , ss.str() );
            cacheKey += ss.str();
        }

        if ( _seenIndexes.count( cacheKey ) )
            return 0;
        _seenIndexes.insert( cacheKey );

        insert( Namespace( ns.c_str() ).getSisterNS( "system.indexes"  ).c_str() , toSave.obj() );
        return 1;
    }

    void DBClientBase::resetIndexCache() {
        _seenIndexes.clear();
    }

    /* -- DBClientCursor ---------------------------------------------- */

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, BSONObj *fieldsToReturn, int queryOptions, Message &toSend ) {
        // see query.h for the protocol we are using here.
        BufBuilder b;
        int opts = queryOptions;
        assert( (opts&Option_ALLMASK) == opts );
        b.append(opts);
        b.append(ns.c_str());
        b.append(nToSkip);
        b.append(nToReturn);
        query.appendSelfToBufBuilder(b);
        if ( fieldsToReturn )
            fieldsToReturn->appendSelfToBufBuilder(b);
        toSend.setData(dbQuery, b.buf(), b.len());
    }

    void DBClientConnection::say( Message &toSend ) {
        checkConnection();
        try { 
            port().say( toSend );
        } catch( SocketException & ) { 
            failed = true;
            throw;
        }
    }

    void DBClientConnection::sayPiggyBack( Message &toSend ) {
        port().piggyBack( toSend );
    }

    bool DBClientConnection::call( Message &toSend, Message &response, bool assertOk ) {
        /* todo: this is very ugly messagingport::call returns an error code AND can throw 
                 an exception.  we should make it return void and just throw an exception anytime 
                 it fails
        */
        try { 
            if ( !port().call(toSend, response) ) {
                failed = true;
                if ( assertOk )
                    massert("dbclient error communicating with server", false);
                return false;
            }
        }
        catch( SocketException & ) { 
            failed = true;
            throw;
        }
        return true;
    }

    void DBClientConnection::checkResponse( const char *data, int nReturned ) {
        /* check for errors.  the only one we really care about at
         this stage is "not master" */
        if ( clientPaired && nReturned ) {
            BSONObj o(data);
            BSONElement e = o.firstElement();
            if ( strcmp(e.fieldName(), "$err") == 0 &&
                    e.type() == String && strncmp(e.valuestr(), "not master", 10) == 0 ) {
                clientPaired->isntMaster();
            }
        }
    }

    bool DBClientCursor::init() {
        Message toSend;
        assembleRequest( ns, query, nToReturn, nToSkip, fieldsToReturn, opts, toSend );
        if ( !connector->call( toSend, *m, false ) )
            return false;

        dataReceived();
        return true;
    }

    void DBClientCursor::requestMore() {
        assert( cursorId && pos == nReturned );

        BufBuilder b;
        b.append(opts);
        b.append(ns.c_str());
        b.append(nToReturn);
        b.append(cursorId);

        Message toSend;
        toSend.setData(dbGetMore, b.buf(), b.len());
        auto_ptr<Message> response(new Message());
        connector->call( toSend, *response );

        m = response;
        dataReceived();
    }

    void DBClientCursor::dataReceived() {
        QueryResult *qr = (QueryResult *) m->data;
        if ( qr->resultFlags() & QueryResult::ResultFlag_CursorNotFound ) {
            // cursor id no longer valid at the server.
            assert( qr->cursorId == 0 );
            cursorId = 0; // 0 indicates no longer valid (dead)
        }
        if ( cursorId == 0 ) {
            // only set initially: we don't want to kill it on end of data
            // if it's a tailable cursor
            cursorId = qr->cursorId;
        }
        nReturned = qr->nReturned;
        pos = 0;
        data = qr->data();

        connector->checkResponse( data, nReturned );
        /* this assert would fire the way we currently work:
            assert( nReturned || cursorId == 0 );
        */
    }

    bool DBClientCursor::more() {
        if ( pos < nReturned )
            return true;

        if ( cursorId == 0 )
            return false;

        requestMore();
        return pos < nReturned;
    }

    BSONObj DBClientCursor::next() {
        assert( more() );
        pos++;
        BSONObj o(data);
        data += o.objsize();
        return o;
    }

    DBClientCursor::~DBClientCursor() {
        if ( cursorId ) {
            BufBuilder b;
            b.append( (int)0 ); // reserved
            b.append( (int)1 ); // number
            b.append( cursorId );

            Message m;
            m.setData( dbKillCursors , b.buf() , b.len() );

            connector->sayPiggyBack( m );
        }

    }

    /* ------------------------------------------------------ */

// "./db testclient" to invoke
    extern BSONObj emptyObj;
    void testClient3() {
        out() << "testClient()" << endl;
//	DBClientConnection c(true);
        DBClientPaired c;
        string err;
        if ( !c.connect("10.211.55.2", "1.2.3.4") ) {
//    if( !c.connect("10.211.55.2", err) ) {
            out() << "testClient: connect() failed" << endl;
        }
        else {
            // temp:
            out() << "test query returns: " << c.findOne("foo.bar", fromjson("{}")).toString() << endl;
        }
again:
        out() << "query foo.bar..." << endl;
        auto_ptr<DBClientCursor> cursor =
            c.query("foo.bar", emptyObj, 0, 0, 0, Option_CursorTailable);
        DBClientCursor *cc = cursor.get();
        if ( cc == 0 ) {
            out() << "query() returned 0, sleeping 10 secs" << endl;
            sleepsecs(10);
            goto again;
        }
        while ( 1 ) {
            bool m;
            try {
                m = cc->more();
            } catch (AssertionException&) {
                out() << "more() asserted, sleeping 10 sec" << endl;
                goto again;
            }
            out() << "more: " << m << " dead:" << cc->isDead() << endl;
            if ( !m ) {
                if ( cc->isDead() )
                    out() << "cursor dead, stopping" << endl;
                else {
                    out() << "Sleeping 10 seconds" << endl;
                    sleepsecs(10);
                    continue;
                }
                break;
            }
            out() << cc->next().toString() << endl;
        }
    }

    /* --- class dbclientpaired --- */

    string DBClientPaired::toString() {
        stringstream ss;
        ss << "state: " << master << '\n';
        ss << "left:  " << left.toStringLong() << '\n';
        ss << "right: " << right.toStringLong() << '\n';
        return ss.str();
    }

#pragma warning(disable: 4355)
    DBClientPaired::DBClientPaired() :
		left(true, this), right(true, this)
    {
        master = NotSetL;
    }
#pragma warning(default: 4355)

    /* find which server, the left or right, is currently master mode */
    void DBClientPaired::_checkMaster() {
        for ( int retry = 0; retry < 2; retry++ ) {
            int x = master;
            for ( int pass = 0; pass < 2; pass++ ) {
                DBClientConnection& c = x == 0 ? left : right;
                try {
                    bool im;
                    BSONObj o;
                    c.isMaster(im, &o);
                    if ( retry )
                        log() << "checkmaster: " << c.toString() << ' ' << o.toString() << '\n';
                    if ( im ) {
                        master = (State) (x + 2);
                        return;
                    }
                }
                catch (AssertionException&) {
                    if ( retry )
                        log() << "checkmaster: caught exception " << c.toString() << '\n';
                }
                x = x^1;
            }
            sleepsecs(1);
        }

        uassert("checkmaster: no master found", false);
    }

    inline DBClientConnection& DBClientPaired::checkMaster() {
        if ( master > NotSetR ) {
            // a master is selected.  let's just make sure connection didn't die
            DBClientConnection& c = master == Left ? left : right;
            if ( !c.isFailed() )
                return c;
            // after a failure, on the next checkMaster, start with the other
            // server -- presumably it took over. (not critical which we check first,
            // just will make the failover slightly faster if we guess right)
            master = master == Left ? NotSetR : NotSetL;
        }

        _checkMaster();
        assert( master > NotSetR );
        return master == Left ? left : right;
    }

    bool DBClientPaired::connect(const char *serverHostname1, const char *serverHostname2) {
        string errmsg;
        bool l = left.connect(serverHostname1, errmsg);
        bool r = right.connect(serverHostname2, errmsg);
        master = l ? NotSetL : NotSetR;
        if ( !l && !r ) // it would be ok to fall through, but checkMaster will then try an immediate reconnect which is slow
            return false;
        try {
            checkMaster();
        }
        catch (AssertionException&) {
            return false;
        }
        return true;
    }

	bool DBClientPaired::auth(const char *dbname, const char *username, const char *pwd, string& errmsg) { 
		DBClientConnection& m = checkMaster();
		if( !m.auth(dbname, username, pwd, errmsg) )
			return false;
		/* we try to authentiate with the other half of the pair -- even if down, that way the authInfo is cached. */
		string e;
		try {
			if( &m == &left ) 
				right.auth(dbname, username, pwd, e);
			else
				left.auth(dbname, username, pwd, e);
		}
		catch( AssertionException&) { 
		}
		return true;
	}

    auto_ptr<DBClientCursor> DBClientPaired::query(const char *a, Query b, int c, int d,
            BSONObj *e, int f)
    {
        return checkMaster().query(a,b,c,d,e,f);
    }

    BSONObj DBClientPaired::findOne(const char *a, Query b, BSONObj *c, int d) {
        return checkMaster().findOne(a,b,c,d);
    }

	void testPaired() { 
		DBClientPaired p;
		log() << "connect returns " << p.connect("localhost:27017", "localhost:27018") << endl;

		//DBClientConnection p(true);
		string errmsg;
		//		log() << "connect " << p.connect("localhost", errmsg) << endl;
		log() << "auth " << p.auth("dwight", "u", "p", errmsg) << endl;

		while( 1 ) { 
			sleepsecs(3);
			try { 
				log() << "findone returns " << p.findOne("dwight.foo", emptyObj).toString() << endl;
				sleepsecs(3);
				BSONObj info;
				bool im;
				log() << "ismaster returns " << p.isMaster(im,&info) << " info: " << info.toString() << endl;
			}
			catch(...) { 
				cout << "caught exception" << endl;
			}
		}
	}

} // namespace mongo
