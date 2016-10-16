#include "MapFile.h"

using namespace std ;

static string insertFeatureSQL(const vector<string> &tags, const string &layerName, const string &geomColumn, const string &srid)
{
    string sql ;

    // collect tags

    sql = "INSERT INTO " ;
    sql += layerName ;
    sql += "(" ;
    sql += geomColumn ;

    for(int j=0 ; j<tags.size() ; j++ )
    {
        sql += ',' ;
        sql += tags[j] ;
    }

    sql += ") SELECT ST_Transform(Geometry, " ;
    sql += srid + ")" ;
    for(int j=0 ; j<tags.size() ; j++ ) {
        sql += ',' ;
        sql += tags[j] ;
    }
    sql += " FROM " + layerName + "_temp" ;

    return sql ;

}


void MapFile::addSHPLayerFeatures(const string layerName, const string &type, const std::vector<string> &tags)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    SQLite::Command cmd(con, insertFeatureSQL(tags, layerName, geomColumn, srid)) ;



        for(int i=0 ; i<doc.nodes.size() ; i++ )
        {
            const OSM::Node &node = doc.nodes[i] ;

            if ( node.ways.empty() ) // no associated ways
            {
                for( int j=0 ; j<tags.size() ; j++ )
                {
                    map<string, string>::const_iterator it = node.tags.find(tags[j]) ;
                    if ( it != node.tags.end() )
                    {
                        string value = (*it).second ;
                        cmd.bind(j+2, value) ;
                    }
                    else
                        cmd.bind(j+2, SQLite::Nil) ;
                }

                gaiaGeomCollPtr geo_pt = gaiaAllocGeomColl();

                geo_pt->Srid = 4326;

                gaiaAddPointToGeomColl (geo_pt, node.lon, node.lat);

                gaiaToSpatiaLiteBlobWkb (geo_pt, &blob, &blob_size);

                gaiaFreeGeomColl (geo_pt);

                cmd.bind(1, blob, blob_size) ;
                cmd.exec() ;
                cmd.clear() ;
                free(blob);
            }
        }

        trans.commit() ;
    }
    else if ( type == "lines" )
    {
        // to make edges we split each way into segments

        SQLite::Transaction trans(*con) ;

        SQLite::Command cmd(*con, insertFeatureSQL(tags, layerName)) ;

        int k=0 ;
        for( int i=0 ; i<doc.ways.size() ; i++ )
        {
            const OSM::Way &way = doc.ways[i] ;

            vector<int> nodes ;

            int c = 0 ;
            int cn = way.nodes.front() ;
            int last = way.nodes.back() ;

            while ( cn != last )
            {
                c++ ;
                nodes.push_back(cn) ;

                cn = way.nodes[c] ;

                int nj = doc.nodes[cn].ways.size() ;
                if ( cn != last && doc.nodes[cn].ways.size() == 1 ) continue ;

                nodes.push_back(cn) ;

                // create segment geometry

                cmd.clear() ;

                for( int j=0 ; j<tags.size() ; j++ )
                {
                    map<string, string>::const_iterator it = way.tags.find(tags[j]) ;

                    if ( it != way.tags.end() )
                    {
                        string value = (*it).second ;
                        cmd.bind(j+2, value) ;
                    }
                    else
                        cmd.bind(j+2, SQLite::Nil) ;
                }

                gaiaGeomCollPtr geo_line = gaiaAllocGeomColl();
                geo_line->Srid = 4326;

                gaiaLinestringPtr ls = gaiaAddLinestringToGeomColl (geo_line, nodes.size());

                for(int j=0 ; j<nodes.size() ; j++)
                {
                    const OSM::Node &node = doc.nodes[nodes[j]] ;

                    gaiaSetPoint (ls->Coords, j, node.lon, node.lat);
                }

                gaiaToSpatiaLiteBlobWkb (geo_line, &blob, &blob_size);

                gaiaFreeGeomColl (geo_line);

                cmd.bind(1, blob, blob_size) ;

                cmd.exec() ;

                free(blob) ;

                nodes.clear() ;
            }

        }

        trans.commit() ;
    }
    else if ( type == "polygons" )
    {
        // look for ways with same start and end point

        SQLite::Transaction trans(*con) ;

        SQLite::Command cmd(*con, insertFeatureSQL(tags, layerName)) ;

        for( int i=0 ; i<doc.ways.size() ; i++ )
        {
            const OSM::Way &way = doc.ways[i] ;

            if ( way.nodes.front() == way.nodes.back() )
            {
                for( int j=0 ; j<tags.size() ; j++ )
                {
                    map<string, string>::const_iterator it = way.tags.find(tags[j]) ;

                    if ( it != way.tags.end() )
                    {
                        string value = (*it).second ;
                        cmd.bind(j+2, value) ;
                    }
                    else
                        cmd.bind(j+2, SQLite::Nil) ;
                }

                gaiaGeomCollPtr geo_poly = gaiaAllocGeomColl();
                geo_poly->Srid = 4326;

                gaiaPolygonPtr poly = gaiaAddPolygonToGeomColl (geo_poly, way.nodes.size()-1, 0);

                for(int j=0 ; j<way.nodes.size()-1 ; j++)
                {
                    const OSM::Node &node = doc.nodes[way.nodes[j]] ;

                    gaiaSetPoint (poly->Exterior->Coords, j, node.lon, node.lat);
                }

                gaiaToSpatiaLiteBlobWkb (geo_poly, &blob, &blob_size);

                gaiaFreeGeomColl (geo_poly);

                cmd.bind(1, blob, blob_size) ;

                cmd.exec() ;

                free(blob) ;

            }

        }

        trans.commit() ;




    }




    db_->release(con) ;
}