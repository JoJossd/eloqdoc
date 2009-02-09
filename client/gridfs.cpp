// gridfs.cpp

#include <fcntl.h>

#include "gridfs.h"

#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif

namespace mongo {

    const unsigned DEFAULT_CHUNK_SIZE = 256 * 1024;

    Chunk::Chunk( BSONObj o ){
        _data = o;
    }

    Chunk::Chunk( BSONElement fileId , int chunkNumber , const char * data , int len ){
        BSONObjBuilder b;
        b.appendAs( fileId , "files_id" );
        b.append( "n" , chunkNumber );
        b.appendBinDataArray( "data" , data , len );
        _data = b.obj();
    }


    GridFS::GridFS( DBClientBase& client , string dbName , string prefix ) : _client( client ){
        _filesNS = dbName + "." + prefix + ".files";
        _chunksNS = dbName + "." + prefix + ".chunks";


        client.ensureIndex( _filesNS , BSON( "filename" << 1 ) );
        client.ensureIndex( _chunksNS , BSON( "files_id" << 1 << "n" << 1 ) );
    }

    GridFS::~GridFS(){
        
    }
    
    BSONElement GridFS::storeFile( string filename ){
        uassert( "file doesn't exist" , boost::filesystem::exists( filename ) );
        
        gridfs_offset length = boost::filesystem::file_size( filename );
        
        BSONObj fileObject;
        {
            BSONObjBuilder b;
            b << "filename" << filename ;
            massert("large files not yet implemented", length <= 0xffffffff);
            b << "length" << (unsigned) length ;
            b << "chunkSize" << DEFAULT_CHUNK_SIZE ;
            
            OID id;
            id.init();
            b.appendOID( "_id" , &id );
            
            fileObject = b.obj();
        }
        
        char buf[DEFAULT_CHUNK_SIZE];
        gridfs_offset pos = 0;
        int fd = open( filename.c_str() , O_RDONLY );
        
        int chunkNumber = 0;
        while ( pos < length ){
            int l = read( fd , buf , MIN( DEFAULT_CHUNK_SIZE , length - pos ) );
            
            Chunk c( fileObject["_id"] , chunkNumber , buf , l );
            _client.insert( _chunksNS.c_str() , c._data );
            
            pos += l;
            chunkNumber++;
        }

        _client.insert( _filesNS.c_str() , fileObject );

        return fileObject["_id"];
    }

    GridFile::GridFile( GridFS * grid , BSONObj obj ){
        _grid = grid;
        _obj = obj;
    }
    
    GridFile GridFS::findFile( string filename){ 
        return findFile( BSON( "filename" << filename ) ); 
    };

    GridFile GridFS::findFile( BSONObj query ){
        return GridFile( this , _client.findOne( _filesNS.c_str() , query ) );
    }

    auto_ptr<DBClientCursor> GridFS::list(){
        return _client.query( _filesNS.c_str() , emptyObj );
    }

    auto_ptr<DBClientCursor> GridFS::list( BSONObj o ){
        return _client.query( _filesNS.c_str() , o );
    }

    Chunk GridFile::getChunk( int n ){
        _exists();
        BSONObjBuilder b;
        b.appendAs( _obj["_id"] , "files_id" );
        b.append( "n" , n );

        BSONObj o = _grid->_client.findOne( _grid->_chunksNS.c_str() , b.obj() );
        assert( ! o.isEmpty() );
        return Chunk(o);
    }

    gridfs_offset GridFile::write( ostream & out ){
        _exists();
        
        const int num = getNumChunks();
        
        for ( int i=0; i<num; i++ ){
            Chunk c = getChunk( i );

            int len;
            const char * data = c.data( len );
            out.write( data , len );
        }

        return getContentLength();
    }
    
    gridfs_offset GridFile::write( string where ){
        ofstream out(where.c_str() , ios::out | ios::binary );
        return write( out );
    }

    void GridFile::_exists(){
        uassert( "doesn't exists" , exists() );
    }
    
}
