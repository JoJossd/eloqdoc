// sharding_balance3.js


s = new ShardingTest( "slow_sharding_balance3" , 2 , 2 , 1 , { chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.eq( 1 , s.config.chunks.count()  , "setup1" );

s.config.settings.find().forEach( printjson )

db = s.getDB( "test" );

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

N = 3000

num = 0;

counts = {}

function doUpdate( includeString ){
    var up = { $inc : { x : 1 } }
    if ( includeString )
        up["$set"] = { s : bigString };
    var myid = Random.randInt( N )
    db.foo.update( { _id : myid } , up , true );

    counts[myid] = ( counts[myid] ? counts[myid] : 0 ) + 1;
}

for ( i=0; i<N*10; i++ ){
    doUpdate( true )
}
db.getLastError();

s.printChunks( "test.foo" )

check( "initial" )

assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function dist(){
    var x = {}
    s.config.chunks.find( { ns : "test.foo" } ).forEach(
        function(z){
            if ( x[z.shard] )
                x[z.shard]++
            else
                x[z.shard] = 1;
        }
    );
    return x;
}

function check( msg ){
    for ( var x in counts ){
        var e = counts[x];
        var z = db.foo.findOne( { _id : parseInt( x ) } )
        
        if ( z && z.x == e )
            continue;
        
        sleep( 10000 );
        
        var y = db.foo.findOne( { _id : parseInt( x ) } )

        if ( y ){
            delete y.s;
        }
        
        assert( z , "couldn't find : " + x + " y:" + tojson(y) + " " + msg )
        assert.eq( e , z.x , "count for : " + x + " y:" + tojson(y) + " " + msg )
    }
}

function diff(){
    doUpdate( false )
    db.getLastError();

    if ( Math.random() > .99 ){
        db.getLastError()
        //check(); // SERVER-1430  TODO
    }

    var x = dist();
    if ( Math.random() > .999 )
        printjson( x )
    return Math.max( x.shard0 , x.shard1 ) - Math.min( x.shard0 , x.shard1 );
}

function sum(){
    var x = dist();
    return x.shard0 + x.shard1;
}

assert.lt( 20 , diff() ,"initial load" );
print( diff() )

assert.soon( function(){
    
    var d = diff();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 1 );
    

s.stop();
