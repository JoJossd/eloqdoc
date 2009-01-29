// dump.cpp

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

#include "../stdafx.h"
#include "../client/dbclient.h"
#include "Tool.h"

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Dump : public Tool {
public:
    Dump() : Tool( "dump" , "*" ){
        add_options()
            ("out,o" , po::value<string>() , "output directory" )
            ;
    }
    
    void doCollection( const string coll , path outputFile ) {
        cout << "\t" << coll << " to " << outputFile.string() << endl;
        
        int out = open( outputFile.string().c_str() , O_WRONLY | O_CREAT | O_TRUNC , 0666 );
        assert( out );
        
        auto_ptr<DBClientCursor> cursor = _conn.query( coll.c_str() , emptyObj );

        int num = 0;
        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            write( out , obj.objdata() , obj.objsize() );
            num++;
        }
        
        cout << "\t\t " << num << " objects" << endl;
        
        close( out );
    }    
    
    void go( const string db , const path outdir ) {
        cout << "DATABASE: " << db << "\t to \t" << outdir.string() << endl;
        
        create_directories( outdir );
        
        string sns = db + ".system.namespaces";

        auto_ptr<DBClientCursor> cursor = _conn.query( sns.c_str() , emptyObj );
        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            if ( obj.toString().find( ".$" ) != string::npos )
                continue;
            
            const string name = obj.getField( "name" ).valuestr();
            const string filename = name.substr( db.size() + 1 );
            
            doCollection( name.c_str() , outdir / ( filename + ".bson" ) );
            
        }
        
    }

    int run(){

        path root( getParam( "out" , "dump" ) );
        string db = _db;
        
        if ( db == "*" ){
            cout << "all dbs" << endl;

            BSONObj res = _conn.findOne( "admin.$cmd" , BUILDOBJ( "listDatabases" << 1 ) );
            BSONObj dbs = res.getField( "databases" ).embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ) {
                string key = *i;
                
                BSONObj dbobj = dbs.getField( key ).embeddedObjectUserCheck();
                
                const char * dbName = dbobj.getField( "name" ).valuestr();
                if ( (string)dbName == "local" )
                    continue;

                go ( dbName , root / dbName );
            }
        }
        else {
            go( db , root / db );
        }
        return 0;
    }
};

int main( int argc , char ** argv ) {
    Dump d;
    return d.main( argc , argv );
}
