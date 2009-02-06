// storage.cpp

#include "stdafx.h"
#include "pdfile.h"
#include "reccache.h"
#include "rec.h"
#include "db.h"

namespace mongo {

BasicRecStore RecCache::tempStore;

void writerThread();

void BasicRecStore::init(const char *fn, unsigned recsize)
{ 
    massert( "compile packing problem recstore?", sizeof(RecStoreHeader) == 8192);
    f.open(fn);
    uassert( string("couldn't open file:")+fn, f.is_open() );
    len = f.len();
    if( len == 0 ) { 
        log() << "creating recstore file " << fn << '\n';
        h.recsize = recsize;
        len = sizeof(RecStoreHeader);
        f.write(0, (const char *) &h, sizeof(RecStoreHeader));
    }    
    else { 
        f.read(0, (char *) &h, sizeof(RecStoreHeader));
        massert(string("recstore recsize mismatch, file:")+fn, h.recsize == recsize);
        massert(string("bad recstore [1], file:")+fn, (h.leof-sizeof(RecStoreHeader)) % recsize == 0);        
        if( h.leof > len ) { 
            stringstream ss;
            ss << "bad recstore, file:" << fn << " leof:" << h.leof << " len:" << len;
            massert(ss.str(), false);
        }
        if( h.cleanShutdown )
            log() << "warning: non-clean shutdown for file " << fn << '\n';
        h.cleanShutdown = 2;
        writeHeader();
    }
#if defined(_RECSTORE)
    boost::thread t(writerThread);
#endif
}


}
