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

#include "storage.h"
#include "jsobj.h"
#include "namespace.h"

namespace mongo {

    /* For the database/server protocol, these objects and functions encapsulate
       the various messages transmitted over the connection.
    */

    class DbMessage {
    public:
        DbMessage(const Message& _m) : m(_m) {
            theEnd = _m.data->_data + _m.data->dataLen();
            int *r = (int *) _m.data->_data;
            reserved = *r;
            r++;
            data = (const char *) r;
            nextjsobj = data;
        }

        const char * getns() {
            return data;
        }
        void getns(Namespace& ns) {
            ns = data;
        }

        int pullInt() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            int i = *((int *)nextjsobj);
            nextjsobj += 4;
            return i;
        }
        long long pullInt64() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            long long i = *((long long *)nextjsobj);
            nextjsobj += 8;
            return i;
        }

        OID* getOID() {
            return (OID *) (data + strlen(data) + 1); // skip namespace
        }

        void getQueryStuff(const char *&query, int& ntoreturn) {
            int *i = (int *) (data + strlen(data) + 1);
            ntoreturn = *i;
            i++;
            query = (const char *) i;
        }

        /* for insert and update msgs */
        bool moreJSObjs() {
            return nextjsobj != 0;
        }
        BSONObj nextJsObj() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            massert( "Remaining data too small for BSON object", theEnd - nextjsobj > 3 );
            BSONObj js(nextjsobj);
            massert( "Invalid object size", js.objsize() > 3 );
            massert( "Next object larger than available space",
                    js.objsize() < ( theEnd - data ) );
            nextjsobj += js.objsize();
            if ( nextjsobj >= theEnd )
                nextjsobj = 0;
            return js;
        }

        const Message& msg() {
            return m;
        }

    private:
        const Message& m;
        int reserved;
        const char *data;
        const char *nextjsobj;
        const char *theEnd;
    };

    /* a request to run a query, received from the database */
    class QueryMessage {
    public:
        const char *ns;
        int ntoskip;
        int ntoreturn;
        int queryOptions;
        BSONObj query;
        auto_ptr< set<string> > fields;

        /* parses the message into the above fields */
        QueryMessage(DbMessage& d) {
            ns = d.getns();
            ntoskip = d.pullInt();
            ntoreturn = d.pullInt();
            query = d.nextJsObj();
            if ( d.moreJSObjs() ) {
                fields = auto_ptr< set<string> >(new set<string>());
                d.nextJsObj().getFieldNames(*fields);
            }
            queryOptions = d.msg().data->dataAsInt();
        }
    };

} // namespace mongo

#include "../client/dbclient.h"

namespace mongo {

    inline void replyToQuery(int queryResultFlags,
                             MessagingPort& p, Message& requestMsg,
                             void *data, int size,
                             int nReturned, int startingFrom = 0,
                             long long cursorId = 0
                            ) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        b.append(data, size);
        QueryResult *qr = (QueryResult *) b.buf();
        qr->resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = cursorId;
        qr->startingFrom = startingFrom;
        qr->nReturned = 1;
        b.decouple();
        Message *resp = new Message();
        resp->setData(qr, true); // transport will free
        p.reply(requestMsg, *resp, requestMsg.data->id);
    }

} // namespace mongo

//#include "bsonobj.h"
#include "instance.h"

namespace mongo {

    /* object reply helper. */
    inline void replyToQuery(int queryResultFlags,
                             MessagingPort& p, Message& requestMsg,
                             BSONObj& responseObj)
    {
        replyToQuery(queryResultFlags,
                     p, requestMsg,
                     (void *) responseObj.objdata(), responseObj.objsize(), 1);
    }

    /* helper to do a reply using a DbResponse object */
    inline void replyToQuery(int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj) {
        BufBuilder b;
        b.skip(sizeof(QueryResult));
        b.append((void*) obj.objdata(), obj.objsize());
        QueryResult* msgdata = (QueryResult *) b.buf();
        b.decouple();
        QueryResult *qr = msgdata;
        qr->resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = 0;
        qr->startingFrom = 0;
        qr->nReturned = 1;
        Message *resp = new Message();
        resp->setData(msgdata, true); // transport will free
        dbresponse.response = resp;
        dbresponse.responseTo = m.data->id;
    }

} // namespace mongo
