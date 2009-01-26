// namespacedetails.cpp

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
#include "db.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "btree.h"
#include <algorithm>
#include <list>
#include "query.h"
#include "json.h"

namespace mongo {

    BSONObj idKeyPattern = fromjson("{\"_id\":1}");

    /* deleted lists -- linked lists of deleted records -- tehy are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    int bucketSizes[] = {
        32, 64, 128, 256, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000,
        0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
        0x400000, 0x800000
    };

//NamespaceIndexMgr namespaceIndexMgr;

    /* returns true if we created (did not exist) during init() */
    bool NamespaceIndex::init(const char *dir, const char *database) {
        boost::filesystem::path path( dir );
        path /= string( database ) + ".ns";

        bool created = !boost::filesystem::exists(path);

        /* if someone manually deleted the datafiles for a database,
           we need to be sure to clear any cached info for the database in
           local.*.
        */
        if ( string("local") != database ) {
            DBInfo i(database);
            i.dbDropped();
        }

        const int LEN = 16 * 1024 * 1024;
        string pathString = path.string();
        void *p = f.map(pathString.c_str(), LEN);
        if ( p == 0 ) {
            problem() << "couldn't open file " << pathString << " terminating" << endl;
            exit(-3);
        }
        ht = new HashTable<Namespace,NamespaceDetails>(p, LEN, "namespace index");
        return created;
    }

    void NamespaceDetails::addDeletedRec(DeletedRecord *d, DiskLoc dloc) {
        {
            // defensive code: try to make us notice if we reference a deleted record
            (unsigned&) (((Record *) d)->data) = 0xeeeeeeee;
        }
        dassert( dloc.drec() == d );
        DEBUGGING out() << "TEMP: add deleted rec " << dloc.toString() << ' ' << hex << d->extentOfs << endl;
        if ( capped ) {
            if ( !deletedList[ 1 ].isValid() ) {
                // Initial extent allocation.  Insert at end.
                d->nextDeleted = DiskLoc();
                if ( deletedList[ 0 ].isNull() )
                    deletedList[ 0 ] = dloc;
                else {
                    DiskLoc i = deletedList[ 0 ];
                    for (; !i.drec()->nextDeleted.isNull(); i = i.drec()->nextDeleted );
                    i.drec()->nextDeleted = dloc;
                }
            } else {
                d->nextDeleted = firstDeletedInCapExtent();
                firstDeletedInCapExtent() = dloc;
            }
        } else {
            int b = bucket(d->lengthWithHeaders);
            DiskLoc& list = deletedList[b];
            DiskLoc oldHead = list;
            list = dloc;
            d->nextDeleted = oldHead;
        }
    }

    /*
       lenToAlloc is WITH header
    */
    DiskLoc NamespaceDetails::alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc) {
        lenToAlloc = (lenToAlloc + 3) & 0xfffffffc;
        DiskLoc loc = _alloc(ns, lenToAlloc);
        if ( loc.isNull() )
            return loc;

        DeletedRecord *r = loc.drec();

        /* note we want to grab from the front so our next pointers on disk tend
        to go in a forward direction which is important for performance. */
        int regionlen = r->lengthWithHeaders;
        extentLoc.set(loc.a(), r->extentOfs);
        assert( r->extentOfs < loc.getOfs() );

        DEBUGGING out() << "TEMP: alloc() returns " << loc.toString() << ' ' << ns << " lentoalloc:" << lenToAlloc << " ext:" << extentLoc.toString() << endl;

        int left = regionlen - lenToAlloc;
        if ( capped == 0 ) {
            if ( left < 24 || left < (lenToAlloc >> 3) ) {
                // you get the whole thing.
                return loc;
            }
        }

        /* split off some for further use. */
        r->lengthWithHeaders = lenToAlloc;
        DiskLoc newDelLoc = loc;
        newDelLoc.inc(lenToAlloc);
        DeletedRecord *newDel = newDelLoc.drec();
        newDel->extentOfs = r->extentOfs;
        newDel->lengthWithHeaders = left;
        newDel->nextDeleted.Null();

        addDeletedRec(newDel, newDelLoc);

        return loc;
    }

    /* for non-capped collections.
       returned item is out of the deleted list upon return
    */
    DiskLoc NamespaceDetails::__stdAlloc(int len) {
        DiskLoc *prev;
        DiskLoc *bestprev = 0;
        DiskLoc bestmatch;
        int bestmatchlen = 0x7fffffff;
        int b = bucket(len);
        DiskLoc cur = deletedList[b];
        prev = &deletedList[b];
        int extra = 5; // look for a better fit, a little.
        int chain = 0;
        while ( 1 ) {
            {
                int a = cur.a();
                if ( a < -1 || a >= 100000 ) {
                    problem() << "~~ Assertion - cur out of range in _alloc() " << cur.toString() <<
                    " a:" << a << " b:" << b << " chain:" << chain << '\n';
                    sayDbContext();
                    if ( cur == *prev )
                        prev->Null();
                    cur.Null();
                }
            }
            if ( cur.isNull() ) {
                // move to next bucket.  if we were doing "extra", just break
                if ( bestmatchlen < 0x7fffffff )
                    break;
                b++;
                if ( b > MaxBucket ) {
                    // out of space. alloc a new extent.
                    return DiskLoc();
                }
                cur = deletedList[b];
                prev = &deletedList[b];
                continue;
            }
            DeletedRecord *r = cur.drec();
            if ( r->lengthWithHeaders >= len &&
                    r->lengthWithHeaders < bestmatchlen ) {
                bestmatchlen = r->lengthWithHeaders;
                bestmatch = cur;
                bestprev = prev;
            }
            if ( bestmatchlen < 0x7fffffff && --extra <= 0 )
                break;
            if ( ++chain > 30 && b < MaxBucket ) {
                // too slow, force move to next bucket to grab a big chunk
                //b++;
                chain = 0;
                cur.Null();
            }
            else {
                if ( r->nextDeleted.getOfs() == 0 ) {
                    problem() << "~~ Assertion - bad nextDeleted " << r->nextDeleted.toString() <<
                    " b:" << b << " chain:" << chain << ", fixing.\n";
                    r->nextDeleted.Null();
                }
                cur = r->nextDeleted;
                prev = &r->nextDeleted;
            }
        }

        /* unlink ourself from the deleted list */
        {
            DeletedRecord *bmr = bestmatch.drec();
            *bestprev = bmr->nextDeleted;
            bmr->nextDeleted.setInvalid(); // defensive.
            assert(bmr->extentOfs < bestmatch.getOfs());
        }

        return bestmatch;
    }

    void NamespaceDetails::dumpDeleted(set<DiskLoc> *extents) {
//	out() << "DUMP deleted chains" << endl;
        for ( int i = 0; i < Buckets; i++ ) {
//		out() << "  bucket " << i << endl;
            DiskLoc dl = deletedList[i];
            while ( !dl.isNull() ) {
                DeletedRecord *r = dl.drec();
                DiskLoc extLoc(dl.a(), r->extentOfs);
                if ( extents == 0 || extents->count(extLoc) <= 0 ) {
                    out() << "  bucket " << i << endl;
                    out() << "   " << dl.toString() << " ext:" << extLoc.toString();
                    if ( extents && extents->count(extLoc) <= 0 )
                        out() << '?';
                    out() << " len:" << r->lengthWithHeaders << endl;
                }
                dl = r->nextDeleted;
            }
        }
//	out() << endl;
    }

    /* combine adjacent deleted records

       this is O(n^2) but we call it for capped tables where typically n==1 or 2!
       (or 3...there will be a little unused sliver at the end of the extent.)
    */
    void NamespaceDetails::compact() {
        assert(capped);

        list<DiskLoc> drecs;

        // Pull out capExtent's DRs from deletedList
        DiskLoc i = firstDeletedInCapExtent();
        for (; !i.isNull() && inCapExtent( i ); i = i.drec()->nextDeleted )
            drecs.push_back( i );
        firstDeletedInCapExtent() = i;

        // This is the O(n^2) part.
        drecs.sort();

        list<DiskLoc>::iterator j = drecs.begin();
        assert( j != drecs.end() );
        DiskLoc a = *j;
        while ( 1 ) {
            j++;
            if ( j == drecs.end() ) {
                DEBUGGING out() << "TEMP: compact adddelrec\n";
                addDeletedRec(a.drec(), a);
                break;
            }
            DiskLoc b = *j;
            while ( a.a() == b.a() && a.getOfs() + a.drec()->lengthWithHeaders == b.getOfs() ) {
                // a & b are adjacent.  merge.
                a.drec()->lengthWithHeaders += b.drec()->lengthWithHeaders;
                j++;
                if ( j == drecs.end() ) {
                    DEBUGGING out() << "temp: compact adddelrec2\n";
                    addDeletedRec(a.drec(), a);
                    return;
                }
                b = *j;
            }
            DEBUGGING out() << "temp: compact adddelrec3\n";
            addDeletedRec(a.drec(), a);
            a = b;
        }
    }

    DiskLoc NamespaceDetails::firstRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? firstExtent : startExtent;
                !i.isNull(); i = i.ext()->xnext ) {
            if ( !i.ext()->firstRecord.isNull() )
                return i.ext()->firstRecord;
        }
        return DiskLoc();
    }

    DiskLoc NamespaceDetails::lastRecord( const DiskLoc &startExtent ) const {
        for (DiskLoc i = startExtent.isNull() ? lastExtent : startExtent;
                !i.isNull(); i = i.ext()->xprev ) {
            if ( !i.ext()->lastRecord.isNull() )
                return i.ext()->lastRecord;
        }
        return DiskLoc();
    }

    DiskLoc &NamespaceDetails::firstDeletedInCapExtent() {
        if ( deletedList[ 1 ].isNull() )
            return deletedList[ 0 ];
        else
            return deletedList[ 1 ].drec()->nextDeleted;
    }

    bool NamespaceDetails::inCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        // We could have a rec or drec, doesn't matter.
        return dl.drec()->myExtent( dl ) == capExtent.ext();
    }

    bool NamespaceDetails::nextIsInCapExtent( const DiskLoc &dl ) const {
        assert( !dl.isNull() );
        DiskLoc next = dl.drec()->nextDeleted;
        if ( next.isNull() )
            return false;
        return inCapExtent( next );
    }

    void NamespaceDetails::advanceCapExtent( const char *ns ) {
        // We want deletedList[ 1 ] to be the last DeletedRecord of the prev cap extent
        // (or DiskLoc() if new capExtent == firstExtent)
        if ( capExtent == lastExtent )
            deletedList[ 1 ] = DiskLoc();
        else {
            DiskLoc i = firstDeletedInCapExtent();
            for (; !i.isNull() && nextIsInCapExtent( i ); i = i.drec()->nextDeleted );
            deletedList[ 1 ] = i;
        }

        capExtent = theCapExtent()->xnext.isNull() ? firstExtent : theCapExtent()->xnext;
        dassert( theCapExtent()->ns == ns );
        theCapExtent()->assertOk();
        capFirstNewRecord = DiskLoc();
    }

    int n_complaints_cap = 0;
    void NamespaceDetails::maybeComplain( const char *ns, int len ) const {
        if ( ++n_complaints_cap < 8 ) {
            out() << "couldn't make room for new record (len: " << len << ") in capped ns " << ns << '\n';
            int i = 0;
            for ( DiskLoc e = firstExtent; !e.isNull(); e = e.ext()->xnext, ++i ) {
                out() << "  Extent " << i;
                if ( e == capExtent )
                    out() << " (capExtent)";
                out() << '\n';
                out() << "    magic: " << hex << e.ext()->magic << dec << " extent->ns: " << e.ext()->ns.buf << '\n';
                out() << "    fr: " << e.ext()->firstRecord.toString() <<
                     " lr: " << e.ext()->lastRecord.toString() << " extent->len: " << e.ext()->length << '\n';
            }
            assert( len * 5 > lastExtentSize ); // assume it is unusually large record; if not, something is broken
        }
    }

    DiskLoc NamespaceDetails::__capAlloc( int len ) {
        DiskLoc prev = deletedList[ 1 ];
        DiskLoc i = firstDeletedInCapExtent();
        DiskLoc ret;
        for (; !i.isNull() && inCapExtent( i ); prev = i, i = i.drec()->nextDeleted ) {
            // We need to keep at least one DR per extent in deletedList[ 0 ],
            // so make sure there's space to create a DR at the end.
            if ( i.drec()->lengthWithHeaders >= len + 24 ) {
                ret = i;
                break;
            }
        }

        /* unlink ourself from the deleted list */
        if ( !ret.isNull() ) {
            if ( prev.isNull() )
                deletedList[ 0 ] = ret.drec()->nextDeleted;
            else
                prev.drec()->nextDeleted = ret.drec()->nextDeleted;
            ret.drec()->nextDeleted.setInvalid(); // defensive.
            assert( ret.drec()->extentOfs < ret.getOfs() );
        }

        return ret;
    }

    void NamespaceDetails::checkMigrate() {
        // migrate old NamespaceDetails format
        if ( capped && capExtent.a() == 0 && capExtent.getOfs() == 0 ) {
            capFirstNewRecord = DiskLoc();
            capFirstNewRecord.setInvalid();
            // put all the DeletedRecords in deletedList[ 0 ]
            for ( int i = 1; i < Buckets; ++i ) {
                DiskLoc first = deletedList[ i ];
                if ( first.isNull() )
                    continue;
                DiskLoc last = first;
                for (; !last.drec()->nextDeleted.isNull(); last = last.drec()->nextDeleted );
                last.drec()->nextDeleted = deletedList[ 0 ];
                deletedList[ 0 ] = first;
                deletedList[ i ] = DiskLoc();
            }
            // NOTE deletedList[ 1 ] set to DiskLoc() in above

            // Last, in case we're killed before getting here
            capExtent = firstExtent;
        }
    }

    /* alloc with capped table handling. */
    DiskLoc NamespaceDetails::_alloc(const char *ns, int len) {
        if ( !capped )
            return __stdAlloc(len);

        // capped.

        // signal done allocating new extents.
        if ( !deletedList[ 1 ].isValid() )
            deletedList[ 1 ] = DiskLoc();

        assert( len < 400000000 );
        int passes = 0;
        DiskLoc loc;

        // delete records until we have room and the max # objects limit achieved.
        dassert( theCapExtent()->ns == ns );
        theCapExtent()->assertOk();
        DiskLoc firstEmptyExtent;
        while ( 1 ) {
            if ( nrecords < max ) {
                loc = __capAlloc( len );
                if ( !loc.isNull() )
                    break;
            }

            // If on first iteration through extents, don't delete anything.
            if ( !capFirstNewRecord.isValid() ) {
                advanceCapExtent( ns );
                if ( capExtent != firstExtent )
                    capFirstNewRecord.setInvalid();
                // else signal done with first iteration through extents.
                continue;
            }

            if ( !capFirstNewRecord.isNull() &&
                    theCapExtent()->firstRecord == capFirstNewRecord ) {
                // We've deleted all records that were allocated on the previous
                // iteration through this extent.
                advanceCapExtent( ns );
                continue;
            }

            if ( theCapExtent()->firstRecord.isNull() ) {
                if ( firstEmptyExtent.isNull() )
                    firstEmptyExtent = capExtent;
                advanceCapExtent( ns );
                if ( firstEmptyExtent == capExtent ) {
                    maybeComplain( ns, len );
                    return DiskLoc();
                }
                continue;
            }

            DiskLoc fr = theCapExtent()->firstRecord;
            theDataFileMgr.deleteRecord(ns, fr.rec(), fr, true);
            compact();
            assert( ++passes < 5000 );
        }

        // Remember first record allocated on this iteration through capExtent.
        if ( capFirstNewRecord.isValid() && capFirstNewRecord.isNull() )
            capFirstNewRecord = loc;

        return loc;
    }

    /* you MUST call when adding an index.  see pdfile.cpp */
    void NamespaceDetails::addingIndex(const char *thisns, IndexDetails& details) {
        assert( nsdetails(thisns) == this );
        assert( &details == &indexes[nIndexes] );
        nIndexes++;
        NamespaceDetailsTransient::get(thisns).addedIndex();
    }

    /* returns index of the first index in which the field is present. -1 if not present.
       (aug08 - this method not currently used)
    */
    int NamespaceDetails::fieldIsIndexed(const char *fieldName) {
        for ( int i = 0; i < nIndexes; i++ ) {
            IndexDetails& idx = indexes[i];
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // e.g., { ts : -1 }
            if ( !idxKey.findElement(fieldName).eoo() )
                return i;
        }
        return -1;
    }

    /* ------------------------------------------------------------------------- */

    map<string,NamespaceDetailsTransient*> NamespaceDetailsTransient::map;
    typedef map<string,NamespaceDetailsTransient*>::iterator ouriter;

    NamespaceDetailsTransient& NamespaceDetailsTransient::get(const char *ns) {
        NamespaceDetailsTransient*& t = map[ns];
        if ( t == 0 )
            t = new NamespaceDetailsTransient(ns);
        return *t;
    }

    void NamespaceDetailsTransient::computeIndexKeys() {
        NamespaceDetails *d = nsdetails(ns.c_str());
        for ( int i = 0; i < d->nIndexes; i++ ) {
//        set<string> fields;
            d->indexes[i].keyPattern().getFieldNames(allIndexKeys);
//        allIndexKeys.insert(fields.begin(),fields.end());
        }
    }

    /* ------------------------------------------------------------------------- */

    /* add a new namespace to the system catalog (<dbname>.system.namespaces).
       options: { capped : ..., size : ... }
    */
    void addNewNamespaceToCatalog(const char *ns, BSONObj *options = 0) {
        log(1) << "New namespace: " << ns << '\n';
        if ( strstr(ns, "system.namespaces") ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            // TODO: fix above should not be strstr!
            return;
        }

        {
            BSONObjBuilder b;
            b.append("name", ns);
            if ( options )
                b.append("options", *options);
            BSONObj j = b.done();
            char database[256];
            nsToClient(ns, database);
            string s = database;
            s += ".system.namespaces";
            theDataFileMgr.insert(s.c_str(), j.objdata(), j.objsize(), true);
        }
    }

} // namespace mongo
