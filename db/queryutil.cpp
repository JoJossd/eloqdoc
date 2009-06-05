// queryutil.cpp

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

#include "btree.h"
#include "pdfile.h"
#include "queryoptimizer.h"

namespace mongo {

    FieldBound::FieldBound( const BSONElement &e ) :
    lower_( minKey.firstElement() ),
    lowerInclusive_( true ),
    upper_( maxKey.firstElement() ),
    upperInclusive_( true ) {
        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const char *r = e.simpleRegex();
            if ( r ) {
                lower_ = addObj( BSON( "" << r ) ).firstElement();
                upper_ = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                upperInclusive_ = false;
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
            case BSONObj::Equality:
                lower_ = e;
                upper_ = e;
                break;
            case BSONObj::LT:
                upperInclusive_ = false;
            case BSONObj::LTE:
                upper_ = e;
                break;
            case BSONObj::GT:
                lowerInclusive_ = false;
            case BSONObj::GTE:
                lower_ = e;
                break;
	    case BSONObj::opALL: {
	        massert( "$all requires array", e.type() == Array );
		BSONObjIterator i( e.embeddedObject() );
		if ( i.more() ) {
 		    BSONElement f = i.next();
		    if ( !f.eoo() )
		      lower_ = upper_ = f;
		}
		break;
	    }
	    case BSONObj::opIN: {
                massert( "$in requires array", e.type() == Array );
                BSONElement max = minKey.firstElement();
                BSONElement min = maxKey.firstElement();
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    if ( max.woCompare( f, false ) < 0 )
                        max = f;
                    if ( min.woCompare( f, false ) > 0 )
                        min = f;
                }
                lower_ = min;
                upper_ = max;
            }
            default:
                break;
        }
    }
    
    const FieldBound &FieldBound::operator&=( const FieldBound &other ) {
        int cmp;
        cmp = other.upper_.woCompare( upper_, false );
        if ( cmp == 0 )
            if ( !other.upperInclusive_ )
                upperInclusive_ = false;
        if ( cmp < 0 ) {
            upper_ = other.upper_;
            upperInclusive_ = other.upperInclusive_;
        }
        cmp = other.lower_.woCompare( lower_, false );
        if ( cmp == 0 )
            if ( !other.lowerInclusive_ )
                lowerInclusive_ = false;
        if ( cmp > 0 ) {
            lower_ = other.lower_;
            lowerInclusive_ = other.lowerInclusive_;
        }
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        return *this;
    }
    
    string FieldBound::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldBound::addObj( const BSONObj &o ) {
        objData_.push_back( o );
        return o;
    }
    
    FieldBoundSet::FieldBoundSet( const char *ns, const BSONObj &query ) :
    ns_( ns ),
    query_( query.getOwned() ) {
        BSONObjIterator i( query_ );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( strcmp( e.fieldName(), "$where" ) == 0 )
                continue;
            if ( getGtLtOp( e ) == BSONObj::Equality ) {
                bounds_[ e.fieldName() ] &= FieldBound( e );
            }
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    bounds_[ e.fieldName() ] &= FieldBound( f );
                }                
            }
        }
    }
    
    FieldBound *FieldBoundSet::trivialBound_ = 0;
    FieldBound &FieldBoundSet::trivialBound() {
        if ( trivialBound_ == 0 )
            trivialBound_ = new FieldBound();
        return *trivialBound_;
    }
    
    BSONObj FieldBoundSet::simplifiedQuery( const BSONObj &_fields ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i ) {
                b.append( i->first.c_str(), 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const char *name = e.fieldName();
            const FieldBound &bound = bounds_[ name ];
            if ( bound.equality() )
                b.appendAs( bound.lower(), name );
            else if ( bound.nontrivial() ) {
                BSONObjBuilder c;
                if ( bound.lower().type() != MinKey )
                    c.appendAs( bound.lower(), bound.lowerInclusive() ? "$gte" : "$gt" );
                if ( bound.upper().type() != MaxKey )
                    c.appendAs( bound.upper(), bound.upperInclusive() ? "$lte" : "$lt" );
                b.append( name, c.done() );                
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldBoundSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i ) {
            if ( i->second.equality() ) {
                qp.fieldTypes_[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.upper().type() != MaxKey;
                bool lower = i->second.lower().type() != MinKey;
                if ( upper && lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
} // namespace mongo
