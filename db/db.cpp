// db.cpp : Defines the entry point for the console application.
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

#include "stdafx.h"
#include "db.h"
#include "../grid/message.h"
#include "../util/mmap.h"
#include "../util/hashtab.h"
#include "../util/goodies.h"
#include "pdfile.h"
#include "jsobj.h"
#include "javajs.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "../util/unittest.h"
#include "dbmessage.h"
#include "instance.h"

bool useJNI = true;

/* only off if --nocursors which is for debugging. */
extern bool useCursors;

extern int port;
extern int curOp;
extern string dashDashSource;
extern int opLogging;
extern OpLog _oplog;

extern int ctr;
extern int callDepth;

void closeAllSockets();
void startReplication();
void pairWith(const char *remoteEnd, const char *arb);

struct MyStartupTests {
	MyStartupTests() {
		assert( sizeof(OID) == 12 );
	}
} mystartupdbcpp;

void quicktest() { 
	cout << "quicktest()\n";

	MemoryMappedFile mmf;
	char *m = (char *) mmf.map("/tmp/quicktest", 16384);
	//	cout << "mmf reads: " << m << endl;
	strcpy_s(m, 1000, "hello worldz");
}

QueryResult* emptyMoreResult(long long);


void testTheDb() {
	stringstream ss;

	setClient("sys.unittest.pdfile");

	/* this is not validly formatted, if you query this namespace bad things will happen */
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);
	theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);

	BSONObj j1((const char *) &js1);
	deleteObjects("sys.unittest.delete", j1, false);
	theDataFileMgr.insert("sys.unittest.delete", &js1, sizeof(js1));
	deleteObjects("sys.unittest.delete", j1, false);
	updateObjects("sys.unittest.delete", j1, j1, true,ss);
	updateObjects("sys.unittest.delete", j1, j1, false,ss);

	auto_ptr<Cursor> c = theDataFileMgr.findAll("sys.unittest.pdfile");
	while( c->ok() ) {
		Record* r = c->_current();
		c->advance();
	}
	cout << endl;

	client = 0;
}

MessagingPort *grab = 0;
void connThread();

class OurListener : public Listener { 
public:
	OurListener(int p) : Listener(p) { }
	virtual void accepted(MessagingPort *mp) {
		assert( grab == 0 );
		grab = mp;
		boost::thread thr(connThread);
		while( grab )
			sleepmillis(1);
	}
};

void webServerThread();
void pdfileInit();

/* versions
   114 bad memory bug fixed
   115 replay, opLogging
*/
void listen(int port) { 
	const char *Version = "db version: 122";
	problem() << Version << endl;
	pdfileInit();
	//testTheDb();
	log() << "waiting for connections on port " << port << "..." << endl;
	OurListener l(port);
	startReplication();
    boost::thread thr(webServerThread);
	l.listen();
}

class JniMessagingPort : public AbstractMessagingPort { 
public:
	JniMessagingPort(Message& _container) : container(_container) { }
	void reply(Message& received, Message& response, MSGID) {
		container = response;
	}
	void reply(Message& received, Message& response) { 
		container = response;
	}
	Message & container;
};

/* we create one thread for each connection from an app server client.  
   app server will open a pool of threads.
*/
void connThread()
{
	try { 

	MessagingPort& dbMsgPort = *grab;
	grab = 0;

	Message m;
	while( 1 ) { 
		m.reset();
		stringstream ss;

		if( !dbMsgPort.recv(m) ) {
			log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
			dbMsgPort.shutdown();
			break;
		}

		char buf[64];
		time_t_to_String(time(0), buf);
		buf[20] = 0; // don't want the year
		ss << buf;
		//		ss << curTimeMillis() % 10000 << ' ';

		DbResponse dbresponse;
		{
			dblock lk;
			Timer t;
			client = 0;
			curOp = 0;

			int ms;
			bool log = false;
			curOp = m.data->operation();

#if 0
				/* use this if you only want to process operations for a particular namespace.  
				maybe add to cmd line parms or something fancier.
				*/
				DbMessage ddd(m);
				if( strncmp(ddd.getns(), "clusterstock", 12) != 0 ) { 
					static int q;
					if( ++q < 20 ) 
						cout << "TEMP skip " << ddd.getns() << endl;
					goto skip;
				}
#endif

			if( m.data->operation() == dbMsg ) { 
				ss << "msg ";
				char *p = m.data->_data;
				int len = strlen(p);
				if( len > 400 ) 
					cout << curTimeMillis() % 10000 << 
					" long msg received, len:" << len << 
					" ends with: " << p + len - 10 << endl;
				bool end = strcmp("end", p) == 0;
				Message *resp = new Message();
				resp->setData(opReply, "i am fine");
				dbresponse.response = resp;
				dbresponse.responseTo = m.data->id;
				//dbMsgPort.reply(m, resp);
				if( end ) {
					cout << curTimeMillis() % 10000 << "   end msg " << dbMsgPort.farEnd.toString() << endl;
					if( dbMsgPort.farEnd.isLocalHost() ) { 
						dbMsgPort.shutdown();
						sleepmillis(50);
						problem() << "exiting end msg" << endl;
						exit(EXIT_SUCCESS);
					}
					else { 
						cout << "  (not from localhost, ignoring end msg)" << endl;
					}
				}
			}
			else if( m.data->operation() == dbQuery ) { 
				receivedQuery(dbresponse, m, ss, true);
			}
			else if( m.data->operation() == dbInsert ) {
				OPWRITE;
				try { 
					ss << "insert ";
					receivedInsert(m, ss);
				}
				catch( AssertionException& e ) { 
					problem() << " Caught Assertion insert, continuing\n";
					ss << " exception " + e.toString();
				}
			}
			else if( m.data->operation() == dbUpdate ) {
				OPWRITE;
				try { 
					ss << "update ";
					receivedUpdate(m, ss);
				}
				catch( AssertionException& e ) { 
					problem() << " Caught Assertion update, continuing" << endl; 
					ss << " exception " + e.toString();
				}
			}
			else if( m.data->operation() == dbDelete ) {
				OPWRITE;
				try { 
					ss << "remove ";
					receivedDelete(m);
				}
				catch( AssertionException& e ) { 
					problem() << " Caught Assertion receivedDelete, continuing" << endl; 
					ss << " exception " + e.toString();
				}
			}
			else if( m.data->operation() == dbGetMore ) {
				OPREAD;
				DEV log = true;
				ss << "getmore ";
				receivedGetMore(dbresponse, m, ss);
			}
			else if( m.data->operation() == dbKillCursors ) { 
				OPREAD;
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException& e ) { 
					problem() << " Caught Assertion in kill cursors, continuing" << endl; 
					ss << " exception " + e.toString();
				}
			}
			else {
				cout << "    operation isn't supported: " << m.data->operation() << endl;
				assert(false);
			}

			ms = t.millis();
			log = log || ctr++ % 512 == 0;
			DEV log = true;
			if( log || ms > 100 ) {
				ss << ' ' << t.millis() << "ms";
				cout << ss.str().c_str() << endl;
			}
//skip:
			if( client && client->profile >= 1 ) { 
				if( client->profile >= 2 || ms >= 100 ) { 
					// profile it
					profile(ss.str().c_str()+20/*skip ts*/, ms);
				}
			}

		} /* end lock */
		if( dbresponse.response ) 
			dbMsgPort.reply(m, *dbresponse.response, dbresponse.responseTo);
	}

	}
	catch( AssertionException& ) { 
		problem() << "Uncaught AssertionException, terminating" << endl;
		exit(15);
	}
}


void msg(const char *m, const char *address, int port, int extras = 0) {

    SockAddr db(address, port);

//	SockAddr db("127.0.0.1", DBPort);
//	SockAddr db("192.168.37.1", MessagingPort::DBPort);
//	SockAddr db("10.0.21.60", MessagingPort::DBPort);
//	SockAddr db("172.16.0.179", MessagingPort::DBPort);

	MessagingPort p;
	if( !p.connect(db) )
		return;

	const int Loops = 1;
	for( int q = 0; q < Loops; q++ ) {
		Message send;
		Message response;

		send.setData( dbMsg , m);
		int len = send.data->dataLen();

		for( int i = 0; i < extras; i++ )
			p.say(/*db, */send);

		Timer t;
		bool ok = p.call(send, response);
		double tm = t.micros() + 1;
		cout << " ****ok. response.data:" << ok << " time:" << tm / 1000.0 << "ms " << 
			((double) len) * 8 / 1000000 / (tm/1000000) << "Mbps" << endl;
		if(  q+1 < Loops ) {
			cout << "\t\tSLEEP 8 then sending again as a test" << endl;
			sleepsecs(8);
		}
	}
	sleepsecs(1);

	p.shutdown();
}

void msg(const char *m, int extras = 0) { 
    msg(m, "127.0.0.1", DBPort, extras);
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
  psignal( signal, "Signal Received : ");
}

int segvs = 0;
void segvhandler(int x) {
	if( ++segvs > 1 ) {
		signal(x, SIG_DFL);
		if( segvs == 2 ) {
			cout << "\n\n\n got 2nd SIGSEGV" << endl;
			sayDbContext();
		}
		return;
	}
	problem() << "got SIGSEGV " << x << ", terminating :-(" << endl;
	sayDbContext();
//	closeAllSockets();
//	MemoryMappedFile::closeAllFiles();
//	flushOpLog();
	dbexit(14);
}

void mysighandler(int x) { 
   signal(x, SIG_IGN); 
   log() << "got kill or ctrl c signal " << x << ", will terminate after current cmd ends" << endl;
   {
	   dblock lk;
	   problem() << "  now exiting" << endl;
	   exit(12);
   }
}

void setupSignals() {
	assert( signal(SIGINT, mysighandler) != SIG_ERR );
	assert( signal(SIGTERM, mysighandler) != SIG_ERR );
}

#else
void setupSignals() {}
#endif

void initAndListen(int listenPort, const char *dbPath, const char *appserverLoc = null) { 
  if( opLogging ) 
    log() << "opLogging = " << opLogging << endl;
  _oplog.init();

#if !defined(_WIN32)
  assert( signal(SIGSEGV, segvhandler) != SIG_ERR );
#endif

  /*
   * ensure that the dbpath ends with a path delim if not supplied
   * @TODO - the following is embarassing - not sure of there's a clean way to
   * find the platform delim
   */
  
  char endchar = '/';
  const char *endstr = "/";

#if defined(_WIN32)
  endchar = '\\';
  endstr = "\\";
#endif
    
    if (dbPath && dbPath[strlen(dbPath)-1] != endchar) {
    	char *t = (char *) malloc(strlen(dbPath) + 2);

        strcpy(t, dbPath);
        strcat(t, endstr);
        dbPath = t;
    }

    dbpath = dbPath;

#if !defined(_WIN32)
    pid_t pid = 0;
    pid = getpid();
#else
	int pid=0;
#endif
    
    log() << "Mongo DB : starting : pid = " << pid << " port = " << port << " dbpath = " << dbpath 
            <<  " master = " << master << " slave = " << slave << endl;

    if( useJNI ) {
      JavaJS = new JavaJSImpl(appserverLoc);
      javajstest();
    }

	setupSignals();

    listen(listenPort);    
}

//ofstream problems("dbproblems.log", ios_base::app | ios_base::out);
int test2();
void testClient();

int main(int argc, char* argv[], char *envp[] )
{
	DEV cout << "warning: DEV mode enabled\n";

#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif
	srand(curTimeMillis());

	UnitTest::runTests();

	if( argc >= 2 ) {
		if( strcmp(argv[1], "quicktest") == 0 ) {
			quicktest();
			return 0;
		}
		if( strcmp(argv[1], "javatest") == 0 ) {
                        JavaJS = new JavaJSImpl();
			javajstest();
			return 0;
		}
		if( strcmp(argv[1], "test2") == 0 ) {
			return test2();
		}
		if( strcmp(argv[1], "msg") == 0 ) {

		    // msg(argc >= 3 ? argv[2] : "ping");

		    const char *m = "ping";
		    int thePort = DBPort;
		    
		    if (argc >= 3) { 
		        m = argv[2];
		        
		        if (argc > 3) { 
		            thePort = atoi(argv[3]);
		        }
		    }
		    
		    msg(m, "127.0.0.1", thePort);
		    
			return 0;
		}
		if( strcmp(argv[1], "msglots") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping", 1000);
			return 0;
		}
		if( strcmp( argv[1], "testclient") == 0 ) { 
			testClient();
			return 0;
		}
		if( strcmp(argv[1], "zzz") == 0 ) {
			msg(argc >= 3 ? argv[2] : "ping", 1000);
			return 0;
		}
		if( strcmp(argv[1], "run") == 0 ) {
			initAndListen(port, dbpath);
			return 0;
		}
		if( strcmp(argv[1], "longmsg") == 0 ) {
			char buf[800000];
			memset(buf, 'a', 799999);
			buf[799999] = 0;
			buf[799998] = 'b';
			buf[0] = 'c';
			msg(buf);
			return 0;
		}

        /*
         *  *** POST STANDARD SWITCH METHOD - if we don't satisfy, we switch to a 
         *     slightly different mode where "run" is assumed and we can set values
         */
		
        char *appsrvPath = null;
		
        for (int i = 1; i < argc; i++)  {
    
			if( argv[i] == 0 ) continue;
			string s = argv[i];

			if( s == "--port" )
                port = atoi(argv[++i]);
			else if( s == "--nojni" )
				useJNI = false;
			else if( s == "--master" )
				master = true;
			else if( s == "--slave" )
				slave = true;
			else if( s == "--source" ) { 
                /* specifies what the source in local.sources should be */
                dashDashSource = argv[++i];
			}
			else if( s == "--pairwith" ) { 
				pairWith( argv[i+1], argv[i+2] );
                i += 2;
			}
			else if( s == "--dbpath" )
            	dbpath = argv[++i];
            else if( s == "--appsrvpath" )
                appsrvPath = argv[++i];
			else if( s == "--nocursors" ) 
				useCursors = false;
			else if( strncmp(s.c_str(), "--oplog", 7) == 0 ) { 
				int x = s[7] - '0';
				if( x < 0 || x > 7 ) { 
					cout << "can't interpret --oplog setting" << endl;
					exit(13);
				}
				opLogging = x;
			}
        }
        
        initAndListen(port, dbpath, appsrvPath);
        
		exit(0);
	}

	cout << "Mongo db usage:\n";
	cout << "  run               run db" << endl;
	cout << "  msg end [port]    shut down db server listening on port (or default)" << endl;
	cout << "  msg [msg] [port]  send a request to the db server listening on port (or default)" << endl;
	cout << "  msglots           send a bunch of test messages, and then wait for answer on the last one" << endl;
	cout << "  longmsg           send a long test message to the db server" << endl;
	cout << "  quicktest         just check basic assertions and exit" << endl;
	cout << "  test2             run test2() - see code" << endl;
	cout << "\nOptions:" << endl;
	cout << " --port <portno>  --dbpath <root> --appsrvpath <root of appsrv>" << endl;
	cout << " --nocursors  --nojni" << endl;
	cout << " --oplog<n> 0=off 1=W 2=R 3=both 7=W+some reads" << endl;
    cout << "\nReplication:" << endl;
	cout << " --master\n";
    cout << " --slave" << endl;
    cout << " --source <server:port>" << endl;
	cout << " --pairwith <server:port> <arbiter>" << endl;
	cout << endl;
	
	return 0;
}

void foo() { 
  boost::mutex z;
  boost::detail::thread::lock_ops<boost::mutex>::lock(z);
  cout << "inside lock" << endl;
  boost::detail::thread::lock_ops<boost::mutex>::unlock(z);
}
