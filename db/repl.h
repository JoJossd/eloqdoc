// repl.h - replication

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

/* replication data overview

   at the slave:
     local.sources { host: ..., source: ..., syncedTo: ..., dbs: { ... } }

   at the master:
     local.oplog.$<source>
     local.oplog.$main is the default
*/

#pragma once

#include "../client/dbclient.h"

namespace mongo {

    class DBClientConnection;
    class DBClientCursor;
    extern bool slave;
    extern bool master;

    bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, bool slaveOk);

#pragma pack(push,4)
    class OpTime {
        unsigned i;
        unsigned secs;
    public:
        unsigned getSecs() const {
            return secs;
        }
        OpTime(unsigned long long date) {
            reinterpret_cast<unsigned long long&>(*this) = date;
        }
        OpTime(unsigned a, unsigned b) {
            secs = a;
            i = b;
        }
        OpTime() {
            secs = 0;
            i = 0;
        }
        static OpTime now();

        /* We store OpTime's in the database as Javascript Date datatype -- we needed some sort of
           64 bit "container" for these values.  While these are not really "Dates", that seems a
           better choice for now than say, Number, which is floating point.  Note the BinData type
           is perhaps the cleanest choice, lacking a true unsigned64 datatype, but BinData has a
           couple bytes of overhead.
        */
        unsigned long long asDate() const {
            return *((unsigned long long *) &i);
        }
//	  unsigned long long& asDate() { return *((unsigned long long *) &i); }

        bool isNull() {
            return secs == 0;
        }

        string toStringLong() const {
            char buf[64];
            time_t_to_String(secs, buf);
            stringstream ss;
            ss << buf << ' ';
            ss << hex << secs << ':' << i;
            return ss.str();
        }

        string toString() const {
            stringstream ss;
            ss << hex << secs << ':' << i;
            return ss.str();
        }
        bool operator==(const OpTime& r) const {
            return i == r.i && secs == r.secs;
        }
        bool operator!=(const OpTime& r) const {
            return !(*this == r);
        }
        bool operator<(const OpTime& r) const {
            if ( secs != r.secs )
                return secs < r.secs;
            return i < r.i;
        }
    };
#pragma pack(pop)

    /* A replication exception */
    struct SyncException {
    };

    /* A Source is a source from which we can pull (replicate) data.
       stored in collection local.sources.

       Can be a group of things to replicate for several databases.

          { host: ..., source: ..., syncedTo: ..., dbs: { ... } }

       'source' defaults to 'main'; support for multiple source names is
       not done (always use main for now).
    */
    class ReplSource {
        bool resync(string db);
        bool sync_pullOpLog();
        void sync_pullOpLog_applyOperation(BSONObj& op);
        
        auto_ptr<DBClientConnection> conn;
        auto_ptr<DBClientCursor> cursor;

        set<string> addDbNextPass;

        ReplSource();

    protected:
        static void applyOperation(const BSONObj& op);
        
    public:
        bool replacing; // in "replace mode" -- see CmdReplacePeer
        bool paired; // --pair in use
        string hostName;    // ip addr or hostname plus optionally, ":<port>"
        string _sourceName;  // a logical source name.
        string sourceName() const {
            return _sourceName.empty() ? "main" : _sourceName;
        }
        string only; // only a certain db. note that in the sources collection, this may not be changed once you start replicating.

        /* the last time point we have already synced up to. */
        OpTime syncedTo;

        /* list of databases that we have synced.
           we need this so that if we encounter a new one, we know
           to go fetch the old data.
        */
        set<string> dbs;

        int nClonedThisPass;

        static void loadAll(vector<ReplSource*>&);
        static void cleanup(vector<ReplSource*>&);
        ReplSource(BSONObj);
        bool sync();
        void save(); // write ourself to local.sources
        void resetConnection() {
            cursor = auto_ptr<DBClientCursor>(0);
            conn = auto_ptr<DBClientConnection>(0);
        }

        // make a jsobj from our member fields of the form
        //   { host: ..., source: ..., syncedTo: ... }
        BSONObj jsobj();

        bool operator==(const ReplSource&r) const {
            return hostName == r.hostName && sourceName() == r.sourceName();
        }

        bool haveMoreDbsToSync() const {
            return !addDbNextPass.empty();
        }
    };

    /* Write operation to the log (local.oplog.$main)
       "i" insert
       "u" update
       "d" delete
       "c" db cmd
       "db" declares presence of a database (ns is set to the db name + '.')
    */
    void _logOp(const char *opstr, const char *ns, BSONObj& obj, BSONObj *patt, bool *b);
    inline void logOp(const char *opstr, const char *ns, BSONObj& obj, BSONObj *patt = 0, bool *b = 0) {
        if ( master )
            _logOp(opstr, ns, obj, patt, b);
    }

} // namespace mongo
