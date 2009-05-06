// querytests.cpp : query.{h,cpp} unit tests.
//

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

#include "../db/query.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"

#include "dbtests.h"

namespace QueryTests {

    class Base {
    public:
        Base() {
            dblock lk;
            setClient( ns() );
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                auto_ptr< Cursor > c = theDataFileMgr.findAll( ns() );
                vector< DiskLoc > toDelete;
                for(; c->ok(); c->advance() )
                    toDelete.push_back( c->currLoc() );
                for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                    theDataFileMgr.deleteRecord( ns(), i->rec(), *i, false );
            } catch ( ... ) {
                FAIL( "Exception while cleaning up records" );
            }
        }
    protected:
        static const char *ns() {
            return "unittest.querytests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", "index" );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            stringstream indexNs;
            indexNs << ns() << ".system.indexes";
            theDataFileMgr.insert( indexNs.str().c_str(), o.objdata(), o.objsize() );
        }
        static void insert( const char *s ) {
            insert( fromjson( s ) );
        }
        static void insert( const BSONObj &o ) {
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize() );
        }
    };
    
    class CountBasic : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            BSONObj cmd = fromjson( "{\"query\":{}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };
    
    class CountQuery : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"b\",\"x\":\"y\"}" );
            insert( "{\"a\":\"c\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":\"b\"}}" );
            string err;
            ASSERT_EQUALS( 2, runCount( ns(), cmd, err ) );
        }        
    };
    
    class CountFields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"c\":\"d\"}" );
            BSONObj cmd = fromjson( "{\"query\":{},\"fields\":{\"a\":1}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }        
    };

    class CountQueryFields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );
            insert( "{\"d\":\"e\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":\"b\"},\"fields\":{\"a\":1}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }        
    };

    class CountIndexedRegex : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );            
            BSONObj cmd = fromjson( "{\"query\":{\"a\":/^b/}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };
    
    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            mongo::lastError.reset( new LastError() );
        }
        ~ClientBase() {
            mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            client_.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            client_.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !client_.getPrevError().getField( "err" ).isNull();
        }
        DBDirectClient &client() const { return client_; }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient ClientBase::client_;
    
    class BoundedKey : public ClientBase {
    public:
        void run() {
            const char *ns = "querytests.BoundedKey";
            insert( ns, BSON( "a" << 1 ) );
            BSONObjBuilder a;
            a.appendMaxKey( "$lt" );
            BSONObj limit = a.done();
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ) ).isEmpty() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ).hint( BSON( "a" << 1 ) ) ).isEmpty() );
        }
    };
    
    class GetMore : public ClientBase {
    public:
        ~GetMore() {
            client().dropCollection( "querytests.GetMore" );
        }
        void run() {
            const char *ns = "querytests.GetMore";
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            auto_ptr< DBClientCursor > cursor = client().query( ns, BSONObj(), 2 );
            long long cursorId = cursor->getCursorId();
            cursor->decouple();
            cursor.reset();
            cursor = client().getMore( ns, cursorId );
            ASSERT( cursor->more() );
            ASSERT_EQUALS( 3, cursor->next().getIntField( "a" ) );
        }
    };
    
    class ReturnOneOfManyAndTail : public ClientBase {
    public:
        ~ReturnOneOfManyAndTail() {
            client().dropCollection( "querytests.ReturnOneOfManyAndTail" );
        }
        void run() {
            const char *ns = "querytests.ReturnOneOfManyAndTail";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 1, 0, 0, Option_CursorTailable );
            // If only one result requested, a cursor is not saved.
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->more() );
            ASSERT_EQUALS( 1, c->next().getIntField( "a" ) );
        }
    };
    
    class TailNotAtEnd : public ClientBase {
    public:
        ~TailNotAtEnd() {
            client().dropCollection( "querytests.TailNotAtEnd" );
        }
        void run() {
            const char *ns = "querytests.TailNotAtEnd";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            ASSERT( 0 != c->getCursorId() );
            while( c->more() )
                c->next();
            ASSERT( 0 != c->getCursorId() );
            insert( ns, BSON( "a" << 3 ) );
            insert( ns, BSON( "a" << 4 ) );
            insert( ns, BSON( "a" << 5 ) );
            insert( ns, BSON( "a" << 6 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 3, c->next().getIntField( "a" ) );
        }
    };
    
    class EmptyTail : public ClientBase {
    public:
        ~EmptyTail() {
            client().dropCollection( "querytests.EmptyTail" );
        }
        void run() {
            const char *ns = "querytests.EmptyTail";
            ASSERT_EQUALS( 0, client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable )->getCursorId() );
            insert( ns, BSON( "a" << 0 ) );
            ASSERT( 0 != client().query( ns, QUERY( "a" << 1 ).hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable )->getCursorId() );
        }
    };
    
    class TailableDelete : public ClientBase {
    public:
        ~TailableDelete() {
            client().dropCollection( "querytests.TailableDelete" );
        }
        void run() {
            const char *ns = "querytests.TailableDelete";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            client().remove( ns, QUERY( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            ASSERT( !c->more() );
            ASSERT_EQUALS( 0, c->getCursorId() );
        }
    };

    class TailableInsertDelete : public ClientBase {
    public:
        ~TailableInsertDelete() {
            client().dropCollection( "querytests.TailableInsertDelete" );
        }
        void run() {
            const char *ns = "querytests.TailableInsertDelete";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            insert( ns, BSON( "a" << 2 ) );
            client().remove( ns, QUERY( "a" << 1 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "a" ) );
            ASSERT( !c->more() );
        }
    };    
    
    class OplogReplayMode : public ClientBase {
    public:
        ~OplogReplayMode() {
            client().dropCollection( "querytests.OplogReplayMode" );            
        }
        void run() {
            const char *ns = "querytests.OplogReplayMode";
            insert( ns, BSON( "a" << 3 ) );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 1 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, Option_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "a" ) );
            ASSERT( !c->more() );
        }
    };
    
    class BasicCount : public ClientBase {
    public:
        ~BasicCount() {
            client().dropCollection( "querytests.BasicCount" );
        }
        void run() {
            const char *ns = "querytests.BasicCount";
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            count( 0 );
            insert( ns, BSON( "a" << 3 ) );
            count( 0 );
            insert( ns, BSON( "a" << 4 ) );
            count( 1 );
            insert( ns, BSON( "a" << 5 ) );
            count( 1 );
            insert( ns, BSON( "a" << 4 ) );
            count( 2 );
        }
    private:
        void count( unsigned long long c ) const {
            ASSERT_EQUALS( c, client().count( "querytests.BasicCount", BSON( "a" << 4 ) ) );
        }
    };
    
    class ArrayId : public ClientBase {
    public:
        ~ArrayId() {
            client().dropCollection( "querytests.ArrayId" );
        }
        void run() {
            const char *ns = "querytests.ArrayId";
            client().ensureIndex( ns, BSON( "_id" << 1 ) );
            ASSERT( !error() );
            client().insert( ns, fromjson( "{'_id':[1,2]}" ) );
            ASSERT( error() );
        }
    };

    class UnderscoreNs : public ClientBase {
    public:
        ~UnderscoreNs() {
            client().dropCollection( "querytests._UnderscoreNs" );            
        }
        void run() {
            const char *ns = "querytests._UnderscoreNs";
            ASSERT( client().findOne( ns, "{}" ).isEmpty() );
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 1, client().findOne( ns, "{}" ).getIntField( "a" ) );
            ASSERT( !error() );
        }
    };

    class EmptyFieldSpec : public ClientBase {
    public:
        ~EmptyFieldSpec() {
            client().dropCollection( "querytests.EmptyFieldSpec" );            
        }
        void run() {
            const char *ns = "querytests.EmptyFieldSpec";
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, "" ).isEmpty() );
            BSONObj empty;
            ASSERT( !client().findOne( ns, "", &empty ).isEmpty() );            
        }        
    };

    class MultiNe : public ClientBase {
    public:
        ~MultiNe() {
            client().dropCollection( "querytests.Ne" );            
        }
        void run() {
            const char *ns = "querytests.Ne";
            client().insert( ns, fromjson( "{a:[1,2]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{a:{$ne:1}}" ) ).isEmpty() );
            BSONObj spec = fromjson( "{a:{$ne:1,$ne:2}}" );
            ASSERT( client().findOne( ns, spec ).isEmpty() );
        }                
    };
    
    class EmbeddedNe : public ClientBase {
    public:
        ~EmbeddedNe() {
            client().dropCollection( "querytests.NestedNe" );            
        }
        void run() {
            const char *ns = "querytests.NestedNe";
            client().insert( ns, fromjson( "{a:[{b:1},{b:2}]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{'a.b':{$ne:1}}" ) ).isEmpty() );
        }                        
    };

    class AutoResetIndexCache : public ClientBase {
    public:
        ~AutoResetIndexCache() {
            client().dropCollection( "querytests.AutoResetIndexCache" );
        }
        static const char *ns() { return "querytests.AutoResetIndexCache"; }
        static const char *idxNs() { return "querytests.system.indexes"; }
        void index() const { ASSERT( !client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) ).isEmpty() ); }
        void noIndex() const { ASSERT( client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) ).isEmpty() ); }
        void checkIndex() {
            client().ensureIndex( ns(), BSON( "a" << 1 ) );
            index();            
        }
        void run() {
            client().dropDatabase( "querytests" );
            noIndex();
            checkIndex();
            client().dropCollection( ns() );
            noIndex();
            checkIndex();
            client().dropDatabase( "querytests" );
            noIndex();
            checkIndex();
        }
    };

    class UniqueIndex : public ClientBase {
    public:
        ~UniqueIndex() {
            client().dropCollection( "querytests.UniqueIndex" );
        }
        void run() {
            const char *ns = "querytests.UniqueIndex";
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 1U, client().count( ns, BSONObj() ) );
            client().dropCollection( ns );
            client().ensureIndex( ns, BSON( "b" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSONObj() ) );
        }
    };

    class UniqueIndexPreexistingData : public ClientBase {
    public:
        ~UniqueIndexPreexistingData() {
            client().dropCollection( "querytests.UniqueIndexPreexistingData" );
        }
        void run() {
            const char *ns = "querytests.UniqueIndexPreexistingData";
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            ASSERT_EQUALS( 0U, client().count( "querytests.system.indexes", BSON( "ns" << ns << "name" << NE << "_id_" ) ) );
        }
    };
    
    class SubobjectInArray : public ClientBase {
    public:
        ~SubobjectInArray() {
            client().dropCollection( "querytests.SubobjectInArray" );
        }
        void run() {
            const char *ns = "querytests.SubobjectInArray";
            client().insert( ns, fromjson( "{a:[{b:{c:1}}]}" ) );
            ASSERT( !client().findOne( ns, BSON( "a.b.c" << 1 ) ).isEmpty() );
            ASSERT( !client().findOne( ns, fromjson( "{'a.c':null}" ) ).isEmpty() );
        }
    };

    class Size : public ClientBase {
    public:
        ~Size() {
            client().dropCollection( "querytests.Size" );
        }
        void run() {
            const char *ns = "querytests.Size";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( client().query( ns, QUERY( "a" << SIZE << 3 ).hint( BSON( "a" << 1 ) ) )->more() );
        }        
    };
    
    class FullArray : public ClientBase {
    public:
        ~FullArray() {
            client().dropCollection( "querytests.IndexedArray" );
        }
        void run() {
            const char *ns = "querytests.IndexedArray";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            ASSERT( !client().query( ns, Query( "{a:[1,2,3]}" ) )->more() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( !client().query( ns, Query( "{a:[1,2,3]}" ).hint( BSON( "a" << 1 ) ) )->more() );
        }        
    };

    class InsideArray : public ClientBase {
    public:
        ~InsideArray() {
            client().dropCollection( "querytests.InsideArray" );
        }
        void run() {
            const char *ns = "querytests.InsideArray";
            client().insert( ns, fromjson( "{a:[[1],2]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" );
        }        
    private:
        void check( const string &hintField ) {
            const char *ns = "querytests.InsideArray";
            ASSERT( !client().query( ns, Query( "{a:[[1],2]}" ).hint( BSON( hintField << 1 ) ) )->more() );            
            ASSERT( client().query( ns, Query( "{a:[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );            
            ASSERT( client().query( ns, Query( "{a:2}" ).hint( BSON( hintField << 1 ) ) )->more() );            
            ASSERT( !client().query( ns, Query( "{a:1}" ).hint( BSON( hintField << 1 ) ) )->more() );            
        }
    };

    class IndexInsideArrayCorrect : public ClientBase {
    public:
        ~IndexInsideArrayCorrect() {
            client().dropCollection( "querytests.IndexInsideArrayCorrect" );
        }
        void run() {
            const char *ns = "querytests.IndexInsideArrayCorrect";
            client().insert( ns, fromjson( "{'_id':1,a:[1]}" ) );
            client().insert( ns, fromjson( "{'_id':2,a:[[1]]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 2, client().query( ns, Query( "{a:[1]}" ).hint( BSON( "a" << 1 ) ) )->next().getIntField( "_id" ) );
        }        
    };
    
    class SubobjArr : public ClientBase {
    public:
        ~SubobjArr() {
            client().dropCollection( "querytests.SubobjArr" );
        }
        void run() {
            const char *ns = "querytests.SubobjArr";
            client().insert( ns, fromjson( "{a:[{b:[1]}]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" );
        }        
    private:
        void check( const string &hintField ) {
            const char *ns = "querytests.SubobjArr";
            ASSERT( !client().query( ns, Query( "{'a.b':1}" ).hint( BSON( hintField << 1 ) ) )->more() );            
            ASSERT( client().query( ns, Query( "{'a.b':[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );            
        }
    };

    class MinMax : public ClientBase {
    public:
        MinMax() : ns( "querytests.MinMax" ) {}
        ~MinMax() {
            client().dropCollection( "querytests.MinMax" );
        }
        void run() {
            client().ensureIndex( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 2 ) );
            
            ASSERT_EQUALS( 4, count( client().query( ns, BSONObj() ) ) );
            BSONObj hints[] = { BSONObj(), BSON( "a" << 1 << "b" << 1 ) };
            for( int i = 0; i < 2; ++i ) {
                check( 0, 0, 3, 3, 4, hints[ i ] );
                check( 1, 1, 2, 2, 4, hints[ i ] );
                check( 1, 2, 2, 2, 3, hints[ i ] );
                check( 1, 2, 2, 1, 2, hints[ i ] );

                auto_ptr< DBClientCursor > c = query( 1, 2, 2, 1, hints[ i ] );
                BSONObj obj = c->next();
                ASSERT_EQUALS( 1, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 2, obj.getIntField( "b" ) );
                obj = c->next();
                ASSERT_EQUALS( 2, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 1, obj.getIntField( "b" ) );
                ASSERT( !c->more() );
            }
        }
    private:
        auto_ptr< DBClientCursor > query( int minA, int minB, int maxA, int maxB, const BSONObj &hint ) {
            Query q;
            q = q.min( BSON( "a" << minA << "b" << minB ) ).max( BSON( "a" << maxA << "b" << maxB ) );
            if ( !hint.isEmpty() )
                q.hint( hint );
            return client().query( ns, q );
        }
        void check( int minA, int minB, int maxA, int maxB, int expectedCount, const BSONObj &hint = empty_ ) {
            ASSERT_EQUALS( expectedCount, count( query( minA, minB, maxA, maxB, hint ) ) );
        }
        int count( auto_ptr< DBClientCursor > c ) {
            int ret = 0;
            while( c->more() ) {
                ++ret;
                c->next();
            }
            return ret;
        }
        const char *ns;
        static BSONObj empty_;
    };
    BSONObj MinMax::empty_;
    
    class DirectLocking : public ClientBase {
    public:
        void run() {
            dblock lk;
            setClient( "foo.bar" );
            client().remove( "a.b", BSONObj() );
            ASSERT_EQUALS( "foo", database->name );
        }
        const char *ns;
    };
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< CountBasic >();
            add< CountQuery >();
            add< CountFields >();
            add< CountQueryFields >();
            add< CountIndexedRegex >();
            add< BoundedKey >();
            add< GetMore >();
            add< ReturnOneOfManyAndTail >();
            add< TailNotAtEnd >();
            add< EmptyTail >();
            add< TailableDelete >();
            add< TailableInsertDelete >();
            add< OplogReplayMode >();
            add< ArrayId >();
            add< UnderscoreNs >();
            add< EmptyFieldSpec >();
            add< MultiNe >();
            add< EmbeddedNe >();
            add< AutoResetIndexCache >();
            add< UniqueIndex >();
            add< UniqueIndexPreexistingData >();
            add< SubobjectInArray >();
            add< Size >();
            add< FullArray >();
            add< InsideArray >();
            add< IndexInsideArrayCorrect >();
            add< SubobjArr >();
            add< MinMax >();
            add< DirectLocking >();
        }
    };
    
} // namespace QueryTests

UnitTest::TestPtr queryTests() {
    return UnitTest::createSuite< QueryTests::All >();
}
