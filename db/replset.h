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

#pragma once

extern int port;

/* ReplPair is a pair of db servers replicating to one another and cooperating.

   Only one member of the pair is active at a time; so this is a smart master/slave
   configuration basically.

   You may read from the slave at anytime though (if you don't mind the slight lag).

   todo: Could be extended to be more than a pair, thus the name 'Set' -- for example, 
   a set of 3...
*/

class ReplPair { 
public:
    enum { 
        State_CantArb = -3,
        State_Confused = -2,
        State_Negotiating = -1,
        State_Slave = 0,
        State_Master = 1
    };

    int state;
    string info; // commentary about our current state
    string arbHost;  // "-" for no arbiter.  "host[:port]"
	int remotePort;
	string remoteHost;
	string remote; // host:port if port specified.
    int date; // -1 not yet set; 0=slave; 1=master

	ReplPair(const char *remoteEnd, const char *arbiter);

    bool dominant(const string& myname) { 
        if( myname == remoteHost )
            return port > remotePort;
        return myname > remoteHost;
    }

    void setMaster(int n, const char *_comment = "") { 
        info = _comment;
        if( n == state ) 
            return;
        log() << "pair: setting master=" << n << " was " << state << '\n';
        state = n;
    }

    /* negotiate with our peer who is master */
    void negotiate(DBClientConnection *conn);

    /* peer unreachable, try our arbitrator */
    void arbitrate();
};

extern ReplPair *replPair;

/* we should not allow most operations when not the master 
   also we report not master if we are "dead"
*/
inline bool isMaster() { 
    if( replPair == 0 ) return true;
    if( client->dead ) return client->name == "local";
    return replPair->state == ReplPair::State_Master;
}

inline ReplPair::ReplPair(const char *remoteEnd, const char *arb) {
    state = -1;
	remote = remoteEnd;
	remotePort = DBPort;
	remoteHost = remoteEnd;
	const char *p = strchr(remoteEnd, ':');
	if( p ) { 
		remoteHost = string(remoteEnd, p-remoteEnd);
		remotePort = atoi(p+1);
		uassert("bad port #", remotePort > 0 && remotePort < 0x10000 );
		if( remotePort == DBPort )
			remote = remoteHost; // don't include ":27017" as it is default; in case ran in diff ways over time to normalizke the hostname format in sources collection
	}

    uassert("arbiter parm is missing, use '-' for none", arb);
    arbHost = arb;
    uassert("arbiter parm is empty", !arbHost.empty());
}
