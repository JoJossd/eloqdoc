
assert = function( b , msg ){
    if ( b )
        return;
    
    throw "assert failed : " + msg;
}

assert.eq = function( a , b , msg ){
    if ( a == b )
        return;

    if ( a != null && b != null && a.toString() == b.toString() )
        return;

    throw "[" + a + "] != [" + b + "] are not equal : " + msg;
}

assert.neq = function( a , b , msg ){
    if ( a != b )
        return;

    throw "[" + a + "] != [" + b + "] are equal : " + msg;
}

assert.soon = function( f, msg, timeout ) {
    var start = new Date();
    timeout = timeout || 30000
    var last;
    while( 1 ) {
        if ( f() )
            return;
        if ( ( new Date() ).getTime() - start.getTime() > timeout )
            throw "assert.soon failed: " + f + ", msg:" + msg;
        sleep( 200 );
    }
}

assert.throws = function( func , params , msg ){

    try {
        func.apply( null , params );
    }
    catch ( e ){
        return e;
    }

    throw "did not throw exception: " + msg ;
}

assert.commandWorked = function( res , msg ){
    if ( res.ok == 1 )
        return;
    
    throw "command failed: " + tojson( res ) + " : " + msg;
}

assert.commandFailed = function( res , msg ){
    if ( res.ok == 0 )
        return;
    
    throw "command worked when it should have failed: " + tojson( res ) + " : " + msg;
}

assert.isnull = function( what , msg ){
    if ( what == null )
        return;
    
    throw "supposed to null (" + ( msg || "" ) + ") was: " + tojson( what );
}

Object.extend = function( dst , src ){
    for ( var k in src ){
        dst[k] = src[k];
    }
    return dst;
}

argumentsToArray = function( a ){
    var arr = [];
    for ( var i=0; i<a.length; i++ )
        arr[i] = a[i];
    return arr;
}

isString = function( x ){
    return typeof( x ) == "string";
}

isNumber = function(x){
    return typeof( x ) == "number";
}

isObject = function( x ){
    return typeof( x ) == "object";
}

String.prototype.trim = function() {
    return this.replace(/^\s+|\s+$/g,"");
}
String.prototype.ltrim = function() {
    return this.replace(/^\s+/,"");
}
String.prototype.rtrim = function() {
    return this.replace(/\s+$/,"");
}

Date.timeFunc = function( theFunc , numTimes ){

    var start = new Date();
    
    numTimes = numTimes || 1;
    for ( var i=0; i<numTimes; i++ ){
        theFunc.apply( null , argumentsToArray( arguments ).slice( 2 ) );
    }

    return (new Date()).getTime() - start.getTime();
}

Date.prototype.tojson = function(){
    return "\"" + this.toString() + "\"";
}

RegExp.prototype.tojson = RegExp.prototype.toString;

Array.prototype.contains = function( x ){
    for ( var i=0; i<this.length; i++ ){
        if ( this[i] == x )
            return true;
    }
    return false;
}

Array.prototype.unique = function( ){
    var u = [];
    for ( var i=0; i<this.length; i++){
        var o = this[i];
        if ( ! u.contains( o ) )
            u.push( o );
    }
    return u;
}

Array.prototype.tojson = function( sepLines ){
    var s = "[";
    if ( sepLines ) s += "\n";
    for ( var i=0; i<this.length; i++){
        if ( i > 0 ){
            s += ",";
            if ( sepLines ) s += "\n";
        }
        s += tojson( this[i] );
    }
    s += "]";
    if ( sepLines ) s += "\n";
    return s;
}

if ( ! ObjectId.prototype )
    ObjectId.prototype = {}

ObjectId.prototype.toString = function(){
    return this.str;
}

ObjectId.prototype.tojson = function(){
    return "\"" + this.str + "\"";
}

ObjectId.prototype.isObjectId = true;

fork = function() {
    var t = new Thread( function() {} );
    Thread.apply( t, arguments );
    return t;
}

tojson = function( x ){
    if ( x == null || x == undefined )
        return "";
    
    switch ( typeof x ){
        
    case "string": 
        return "\"" + x + "\"";
        
    case "number": 
    case "boolean":
        return "" + x;
            
    case "object":
        return tojsonObject( x );
        
    case "function":
        return x.toString();
        

    default:
        throw "tojson can't handle type " + ( typeof x );
    }
    
}

tojsonObject = function( x ){
    assert.eq( ( typeof x ) , "object" , "tojsonObject needs object, not [" + ( typeof x ) + "]" );
    
    if ( typeof( x.tojson ) == "function" && x.tojson != tojson )
        return x.tojson();

    var s = "{";
    
    var first = true;
    for ( var k in x ){

        var val = x[k];
        if ( val == DB.prototype || val == DBCollection.prototype )
            continue;

        if ( first ) first = false;
        else s += " , ";
        
        s += "\"" + k + "\" : " + tojson( val );
    }

    return s + "}";
}

shellPrint = function( x ){
    it = x;
    if ( x != undefined )
        shellPrintHelper( x );
    
    if ( db ){
        var e = db.getPrevError();
        if ( e.err ) {
	    if( e.nPrev <= 1 )
		print( "error on last call: " + tojson( e.err ) );
	    else
		print( "an error " + tojson(e.err) + " occurred " + e.nPrev + " operations back in the command invocation" );
        }
        db.resetError();
    }
}

printjson = function(x){
    print( tojson( x ) );
}

shellPrintHelper = function( x ){
    
    if ( typeof x != "object" ) 
        return print( x );
    
    var p = x.shellPrint;
    if ( typeof p == "function" )
        return x.shellPrint();

    var p = x.tojson;
    if ( typeof p == "function" )
        print( x.tojson() );
    else
        print( tojson( x ) );
}

execShellLine = function(){
    var res = eval( __line__ );
    if ( typeof( res ) != "undefined" ){
        shellPrintHelper( res );
    }
}

shellHelper = function( command , rest ){
    command = command.trim();
    var args = rest.trim().replace(/;$/,"").split( "\s+" );
    
    if ( ! shellHelper[command] )
        throw "no command [" + command + "]";
    
    return shellHelper[command].apply( null , args );
}

help = shellHelper.help = function(){
    print( "HELP" );
    print( "\t" + "show dbs                     show database names");
    print( "\t" + "show collections             show collections in current database");
    print( "\t" + "show users                   show users in current database");
    print( "\t" + "show profile                 show most recent system.profile entries with time >= 1ms");
    print( "\t" + "use <db name>                set curent database to <db name>" );
    print( "\t" + "db.help()                    help on DB methods");
    print( "\t" + "db.foo.help()                help on collection methods");
    print( "\t" + "db.foo.find()                list objects in collection foo" );
    print( "\t" + "db.foo.find( { a : 1 } )     list objects in foo where a == 1" );
    print( "\t" + "it                           result of the last line evaluated; use to further iterate");
}

shellHelper.use = function( dbname ){
    db = db.getMongo().getDB( dbname );
    print( "switched to db " + db.getName() );
}

shellHelper.show = function( what ){
    assert( typeof what == "string" );
    
    if( what == "profile" ) { 
	if( db.system.profile.count() == 0 ) { 
	    print("db.system.profile is empty");
	    print("Use db.setProfilingLevel(2) will enable profiling");
	    print("Use db.system.profile.find() to show raw profile entries");
	} 
	else { 
	    print(); 
	    db.system.profile.find({ millis : { $gt : 0 } }).sort({$natural:-1}).limit(5).forEach( function(x){print(""+x.millis+"ms " + String(x.ts).substring(0,24)); print(x.info); print("\n");} )
        }
	return "";
    }

    if ( what == "users" )
	return db.system.users.find();

    if ( what == "collections" || what == "tables" ) {
        db.getCollectionNames().forEach( function(x){print(x)} );
	return "";
    }
    
    if ( what == "dbs" ) {
        db.getMongo().getDBNames().sort().forEach( function(x){print(x)} );
	return "";
    }
    
    throw "don't know how to show [" + what + "]";

}


Map = function(){
    print( "warning: Map isn't a good thing to use" );
}

Map.prototype.values = function(){
    // TODO: move this to c?
    var a = [];
    for ( var k in this ){
        var v = this[k];
        if ( v == Map.prototype.values )
            continue;
        a.push( v );
    }
    return a;
}

Math.sigFig = function( x , N ){
    if ( ! N ){
        N = 3;
    }
    var p = Math.pow( 10, N - Math.ceil( Math.log( Math.abs(x) ) / Math.log( 10 )) );
    return Math.round(x*p)/p;
}
