// cloner.cpp - copy a database (export/import basically)

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
#include "pdfile.h"
#include "../client/dbclient.h"
#include "../util/builder.h"
#include "jsobj.h"
#include "query.h"
#include "commands.h"
#include "db.h"
#include "repl.h"

extern int port;

class Cloner: boost::noncopyable { 
	DBClientConnection conn;
	void copy(const char *from_ns, const char *to_ns, bool isindex, bool logForRepl, bool slaveOk);
public:
	Cloner() { }

    /* slaveOk - if true it is ok if the source of the data is !ismaster.  
    */
	bool go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOk);
};

/* for index info object:
     { "name" : "name_1" , "ns" : "foo.index3" , "key" :  { "name" : 1.0 } }
   we need to fix up the value in the "ns" parameter so that the name prefix is correct on a 
   copy to a new name.
*/
BSONObj fixindex(BSONObj o) { 
    BSONObjBuilder b;
    BSONObjIterator i(o);
    while( i.more() ) { 
        BSONElement e = i.next();
        if( e.eoo() )
            break;
        if( string("ns") == e.fieldName() ) {
            uassert("bad ns field for index during dbcopy", e.type() == String);
            const char *p = strchr(e.valuestr(), '.');
            uassert("bad ns field for index during dbcopy [2]", p);
            string newname = database->name + p;
            b.append("ns", newname);
        }
        else
            b.append(e);
    }
    BSONObj res= b.doneAndDecouple();

/*    if( mod ) {
    cout << "before: " << o.toString() << endl;
    o.dump();
    cout << "after:  " << res.toString() << endl;
    res.dump();
    }*/

    return res;
}

/* copy the specified collection 
   isindex - if true, this is system.indexes collection.
*/
void Cloner::copy(const char *from_collection, const char *to_collection, bool isindex, bool logForRepl, bool slaveOk) {
    auto_ptr<DBClientCursor> c;
    {
        dbtemprelease r;
        c = auto_ptr<DBClientCursor>( conn.query(from_collection, emptyObj, 0, 0, 0, slaveOk ? Option_SlaveOk : 0) );\
    }
	assert( c.get() );
    while( 1 ) {
        {
            dbtemprelease r;
            if( !c->more() )
                break;
        }
        BSONObj tmp = c->next();

        /* assure object is valid.  note this will slow us down a good bit. */
        if( !tmp.valid() ) {
            cout << "skipping corrupt object from " << from_collection << '\n';
            continue;
        }

        BSONObj js = tmp;
        if( isindex ) {
            assert( strstr(from_collection, "system.indexes") );
            js = fixindex(tmp);
        }

		theDataFileMgr.insert(to_collection, (void*) js.objdata(), js.objsize());
        if( logForRepl )
            logOp("i", to_collection, js);
    }
}

bool Cloner::go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl, bool slaveOK) { 
    string todb = database->name;
    stringstream a,b;
    a << "localhost:" << port;
    b << "127.0.0.1:" << port;
	if( a.str() == masterHost || b.str() == masterHost ) { 
        if( fromdb == todb ) {
            // guard against an "infinite" loop
            /* if you are replicating, the local.sources config may be wrong if you get this */
            errmsg = "can't clone from self (localhost).";
            return false;
        }
	}
    /* todo: we can put thesee releases inside dbclient or a dbclient specialization.
       or just wait until we get rid of global lcok anyway. 
       */
	string ns = fromdb + ".system.namespaces";
	auto_ptr<DBClientCursor> c;
    {
        dbtemprelease r;
        if( !conn.connect(masterHost, errmsg) )
            return false;
        c = auto_ptr<DBClientCursor>( conn.query(ns.c_str(), emptyObj, 0, 0, 0, slaveOK ? Option_SlaveOk : 0) );
    }
	if( c.get() == 0 ) {
		errmsg = "query failed " + ns;
		return false;
	}

    while( 1 ) {
        {
            dbtemprelease r;
            if( !c->more() )
                break;
        }
		BSONObj collection = c->next();
		BSONElement e = collection.findElement("name");
        if( e.eoo() ) { 
            string s = "bad system.namespaces object " + collection.toString();

            /* temp
            cout << masterHost << endl;
            cout << ns << endl;
            cout << e.toString() << endl;
            exit(1);*/

            massert(s.c_str(), false);
        }
		assert( !e.eoo() );
		assert( e.type() == String );
		const char *from_name = e.valuestr();
        if( strstr(from_name, ".system.") || strchr(from_name, '$') ) {
			continue;
        }
		BSONObj options = collection.getObjectField("options");

        /* change name "<fromdb>.collection" -> <todb>.collection */
        const char *p = strchr(from_name, '.');
        assert(p);
        string to_name = todb + p;

		//if( !options.isEmpty() )
        {
			string err;
			userCreateNS(to_name.c_str(), options, err, logForRepl);
		}
		copy(from_name, to_name.c_str(), false, logForRepl, slaveOK);
	}

	// now build the indexes
	string system_indexes_from = fromdb + ".system.indexes";
	string system_indexes_to = todb + ".system.indexes";
	copy(system_indexes_from.c_str(), system_indexes_to.c_str(), true, logForRepl, slaveOK);

	return true;
}

bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, bool slaveOk)
{
	Cloner c;
	return c.go(masterHost, errmsg, fromdb, logForReplication, slaveOk);
}

/* Usage:
   mydb.$cmd.findOne( { clone: "fromhost" } ); 
*/
class CmdClone : public Command { 
public:
    CmdClone() : Command("clone") { }

    virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        string from = cmdObj.getStringField("clone");
        if( from.empty() ) 
            return false;
        /* replication note: we must logOp() not the command, but the cloned data -- if the slave
           were to clone it would get a different point-in-time and not match.
           */
        return cloneFrom(from.c_str(), errmsg, database->name, /*logForReplication=*/!fromRepl, /*slaveok*/false);
    }
} cmdclone;

/* Usage:
   admindb.$cmd.findOne( { copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db> } );
*/
class CmdCopyDb : public Command { 
public:
    CmdCopyDb() : Command("copydb") { }
    virtual bool adminOnly() { return true; }

    virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        string fromhost = cmdObj.getStringField("fromhost");
        if( fromhost.empty() ) { 
            /* copy from self */
            stringstream ss;
            ss << "localhost:" << port;
            fromhost = ss.str();
        }
        string fromdb = cmdObj.getStringField("fromdb");
        string todb = cmdObj.getStringField("todb");
        if( fromhost.empty() || todb.empty() || fromdb.empty() ) {
            errmsg = "parms missing - {copydb: 1, fromhost: <hostname>, fromdb: <db>, todb: <db>}";
            return false;
        }
        setClient(todb.c_str());
        bool res = cloneFrom(fromhost.c_str(), errmsg, fromdb, /*logForReplication=*/!fromRepl, /*slaveok*/false);
        database = 0;
        return res;
    }
} cmdcopydb;
