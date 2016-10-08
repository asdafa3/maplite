#include "mapsforge_database.hpp"
#include "serializer.hpp"
#include "database.hpp"
#include "tms.hpp"

#include <string>
#include <stdexcept>
#include <iostream>
#include <ctime>

#include <spatialite.h>
using namespace std ;

static void get_geometry_extent(SQLite::Connection &con, const string &desc, float &min_lat, float &min_lon, float &max_lat, float &max_lon) {
    string sql = "SELECT Extent(geom) from geom_" + desc ;
    SQLite::Query q(con, sql) ;

    SQLite::QueryResult res = q.exec() ;

    if ( res ) {
        int blob_size ;
        const char *data = res.getBlob(0, blob_size) ;

        gaiaGeomCollPtr geom = gaiaFromSpatiaLiteBlobWkb ((const unsigned char *)data, blob_size);

        gaiaPolygonPtr poly = geom->FirstPolygon ;
        gaiaRingPtr ring = poly->Exterior ;

        double *c = ring->Coords ;

        for( uint i=0 ; i<ring->Points ; i++ ) {
            double lon = *c++, lat = *c++ ;

            min_lat = std::min<float>(min_lat, lat) ;
            min_lon = std::min<float>(min_lon, lon) ;
            max_lat = std::max<float>(max_lat, lat) ;
            max_lon = std::max<float>(max_lon, lon) ;
        }
    }
}

static void sort_histogram(const map<string, uint64_t> &hist, vector<string> &tags) {
    vector<uint64_t> freq ;

    for( const auto &p: hist ) {
        tags.push_back(p.first) ;
        freq.push_back(p.second) ;
    }

    std::sort(tags.begin(), tags.end(),
              [hist](const string &a, const string &b) -> bool  {
        uint64_t fa = hist.find(a)->second ;
        uint64_t fb = hist.find(b)->second ;
        return fa > fb;
    }) ;
}

static bool get_poi_tags(SQLite::Database &db, std::vector<string> &pois) {
    try {
        SQLite::Session session(&db) ;
        SQLite::Connection &con = session.handle() ;

        map<string, uint64_t> tag_hist ;

        string sql = "SELECT key from kv JOIN geom_pois ON geom_pois.osm_id = kv.osm_id;" ;
        SQLite::Query q(con, sql) ;

        SQLite::QueryResult res = q.exec() ;

        while ( res ) {

            string tag = res.get<string>(0);

            auto it = tag_hist.find(tag) ;
            if ( it == tag_hist.end() )
                tag_hist.insert(make_pair(tag, 1)) ;
            else
                it->second ++ ;

            res.next() ;
        }

        sort_histogram(tag_hist, pois) ;

        return true ;
    }
    catch ( SQLite::Exception &e ) {
        cerr << e.what() << endl ;
        return false ;
    }
}

static bool get_way_tags(SQLite::Database &db, std::vector<string> &ways) {
    try {
        SQLite::Session session(&db) ;
        SQLite::Connection &con = session.handle() ;

        string sql = "SELECT key from kv JOIN geom_lines, geom_polygons, geom_relations ON geom_lines.osm_id = kv.osm_id OR geom_polygons.osm_id = kv.osm_id OR geom_relations.osm_id = kv.osm_id;" ;
        SQLite::Query q(con, sql) ;

        SQLite::QueryResult res = q.exec() ;

        map<string, uint64_t> tag_hist ;

        while ( res ) {

            string tag = res.get<string>(0);

            auto it = tag_hist.find(tag) ;
            if ( it == tag_hist.end() )
                tag_hist.insert(make_pair(tag, 1)) ;
            else
                it->second ++ ;

            res.next() ;
        }

        sort_histogram(tag_hist, ways) ;

        return true ;
    }
    catch ( SQLite::Exception &e ) {
        cerr << e.what() << endl ;
        return false ;
    }
}

void MapFile::setBoundingBox(float min_lat, float min_lon, float max_lat, float max_lon) {
    info_.min_lat_ = min_lat ;
    info_.min_lon_ = min_lon ;
    info_.max_lat_ = max_lat ;
    info_.max_lon_ = max_lon ;
}

void MapFile::setBoundingBoxFromGeometries(SQLite::Database &db) {

    info_.min_lon_ = info_.min_lat_ = std::numeric_limits<float>::max() ;
    info_.max_lat_ = info_.max_lon_ = -std::numeric_limits<float>::max() ;

    SQLite::Session session(&db) ;
    SQLite::Connection &con = session.handle() ;

    get_geometry_extent(con, "pois", info_.min_lat_, info_.min_lon_, info_.max_lat_, info_.max_lon_) ;
    get_geometry_extent(con, "lines", info_.min_lat_, info_.min_lon_, info_.max_lat_, info_.max_lon_) ;
    get_geometry_extent(con, "relations", info_.min_lat_, info_.min_lon_, info_.max_lat_, info_.max_lon_) ;
    get_geometry_extent(con, "polygons", info_.min_lat_, info_.min_lon_, info_.max_lat_, info_.max_lon_) ;
}

void MapFile::setStartPosition(float lat, float lon) {
    info_.start_lat_ = lat ;
    info_.start_lon_ = lon ;
    info_.flags_ |= 0x40 ;
}

void MapFile::setStartZoom(uint8_t zoom) {
    info_.start_zoom_level_ = zoom ;
    info_.flags_ |= 0x20 ;
}

void MapFile::setPreferredLanguages(const std::string &langs) {
    info_.lang_preference_ = langs ;
    info_.flags_ |= 0x10 ;
}

void MapFile::setComment(const std::string &comment) {
    info_.comment_ = comment ;
    info_.flags_ |= 0x08 ;
}

void MapFile::setCreator(const std::string &creator) {
    info_.created_by_ = creator ;
    info_.flags_ |= 0x04 ;
}

void MapFile::setDebug(bool debug) {
    has_debug_info_ = debug ;
    if ( debug ) info_.flags_ |= 0x80 ;
}


void MapFile::create(const std::string &file_path) {

    strm_.open(file_path.c_str(),  ios::out | ios::binary) ;

    if ( !strm_ ) throw std::runtime_error("error in file creation") ;

    // initialize info

    info_.version_ = 4 ;
    info_.date_ = time(0) * 1000 ;
    info_.file_size_ = 0 ;
    info_.tile_sz_ = 256 ;
    info_.projection_ = "Mercator" ;
}

void MapFile::write(SQLite::Database &db, WriteOptions &options) {

    setDebug(options.debug_);
    writeHeader(db, options) ;
    writeSubFiles(db, options) ;
}



void MapFile::writeHeader(SQLite::Database &db, WriteOptions &options)
{
    MapFileOSerializer so(strm_) ;

    // write the magic bytes

    const char *header = "mapsforge binary OSM" ;
    so.write_bytes((uint8_t *)header, 20) ;
    so.write_uint32(0) ; // header length unknown so far

    writeMapInfo() ;

    // get frequency sorted POI and way tags from the database
    get_poi_tags(db, poi_tags_) ;
    get_way_tags(db, way_tags_) ;

    writeTagList(poi_tags_) ;
    writeTagList(way_tags_) ;

    writeSubFileInfo(options) ;

    // now we finished with the header write header length
    int32_t cp = strm_.tellg() ;
    strm_.seekg(20) ;
    info_.header_size_ = cp - 20 ;
    so.write_uint32(info_.header_size_) ;
    strm_.seekg(cp) ;
}

void MapFile::writeMapInfo() {
    MapFileOSerializer s(strm_) ;

    s.write_uint32(info_.version_) ;
    s.write_uint32(info_.file_size_) ;
    s.write_uint64(info_.date_) ;

    s.write_int32(round(info_.min_lat_ * 1.0e6)) ;
    s.write_int32(round(info_.min_lon_ * 1.0e6)) ;
    s.write_int32(round(info_.max_lat_ * 1.0e6)) ;
    s.write_int32(round(info_.max_lon_ * 1.0e6)) ;

    s.write_int16(info_.tile_sz_) ;
    s.write_utf8(info_.projection_) ;
    s.write_uint8(info_.flags_) ;

    has_debug_info_ = info_.flags_ & 0x80 ;

    if ( info_.flags_ & 0x40 ) {
        s.write_int32(round(info_.start_lat_ * 1.0e6) ) ;
        s.write_int32(round(info_.start_lon_ * 1.0e6) ) ;
    }

    if ( info_.flags_ & 0x20 )
        s.write_uint8(info_.start_zoom_level_) ;

    if ( info_.flags_ & 0x10 )
        s.write_utf8(info_.lang_preference_) ;

    if ( info_.flags_ & 0x08 )
        s.write_utf8(info_.comment_) ;

    if ( info_.flags_ & 0x04 )
        s.write_utf8(info_.created_by_) ;

}

void MapFile::writeTagList(const vector<string> &tags)
{
    MapFileOSerializer s(strm_) ;

    s.write_uint16(tags.size()) ;

    for( const auto &tag: tags)
        s.write_utf8(tag) ;
}


void MapFile::writeSubFileInfo(const WriteOptions &options)
{

    MapFileOSerializer s(strm_) ;

    // create subfile structure from configuration string

    info_.min_zoom_level_ = std::numeric_limits<uint8_t>::max();
    info_.max_zoom_level_ = std::numeric_limits<uint8_t>::min();

    const vector<uint8_t> &conf = options.zoom_interval_conf_ ;

    for( uint i=0 ; i<conf.size() ; ) {
        SubFileInfo info ;
        info.base_zoom_ = conf[i++] ;
        info.min_zoom_ = conf[i++] ;
        info.max_zoom_ = conf[i++] ;

        info_.min_zoom_level_ = std::min(info_.min_zoom_level_, info.min_zoom_) ;
        info_.max_zoom_level_ = std::max(info_.max_zoom_level_, info.max_zoom_) ;

        int32_t min_ty, max_ty ;

        tms::tilesWithinBounds(info_.min_lat_, info_.min_lon_, info_.max_lat_, info_.max_lon_, info.base_zoom_,
                               info.min_tx_, min_ty, info.max_tx_, max_ty) ;

        // comvert to Google tile coordinates
        info.max_ty_ = (1 << info.base_zoom_) - min_ty - 1 ;
        info.min_ty_ = (1 << info.base_zoom_) - max_ty - 1 ;

        sub_files_.push_back(info) ;
    }

    s.write_uint8(sub_files_.size()) ;

    for( SubFileInfo &info: sub_files_ ) {
        s.write_uint8(info.base_zoom_) ;
        s.write_uint8(info.min_zoom_) ;
        s.write_uint8(info.max_zoom_) ;
        s.write_uint64(info.offset_) ;
        s.write_uint64(info.size_) ;
        info.foffset_ = strm_.tellg() ; // save location of the field to populate it later
    }
}

void MapFile::writeSubFiles(SQLite::Database &db, const WriteOptions &options)
{
    MapFileOSerializer s(strm_) ;

    for( SubFileInfo &info: sub_files_ ) {

        info.offset_ = strm_.tellg() ;

        if ( has_debug_info_ ) {
            const char *signature = "+++IndexStart+++" ;
            s.write_bytes((uint8_t *)signature, 16) ;
        }

        // determine tiles covered by the bounding box on the base zoom level

        uint32_t rows = info.max_ty_ - info.min_ty_ + 1 ;
        uint32_t cols = info.max_tx_ - info.min_tx_ + 1 ;
        uint64_t tile_count = rows * cols ;

        info.index_.resize(tile_count) ;

        // the tile index table contains tile_count * 5 bytes ( we will fill this later )

        strm_.seekg(tile_count * 5, ios::cur) ;

        uint64_t current_pos = strm_.tellg() ;

        for( uint j=0 ; j<tile_count ; j++ ) {
            info.index_[j] = current_pos ;

            int32_t row = j / cols ;
            int32_t col = j % cols ;

            int32_t tx = col + info.min_tx_ ;
            int32_t ty = row + info.min_ty_ ;
            int32_t tz = info.base_zoom_ ;

            uint64_t bytes = writeTileData(tx, ty, tz, info.min_zoom_, info.max_zoom_, db, options) ;


            /*
            bool is_sea_tile = ( tile_offset & 0x8000000000LL ) != 0 ;

            std::shared_ptr<TileData> data(new TileData(tx, ty, info.base_zoom_, is_sea_tile)) ;

            if ( !is_sea_tile ) {
                tile_offset = tile_offset & 0x7FFFFFFFFFLL ;
                readTileData(info, tile_offset, data) ;
            }
        }
        */
        }

    }
}


static string make_bbox_query(const std::string &tableName, const BBox &bbox, int min_zoom,
                              int max_zoom, bool clip, double buffer, double tol)
{
    stringstream sql ;

    sql.precision(16) ;

    sql << "SELECT g.osm_id, kv.key, kv.val, kv.zoom_min, kv.zoom_max, " ;

    if ( tol != 0.0 )
        sql << "SimplifyPreserveTopology(" ;

    if ( clip ) {
        sql << "ST_ForceLHR(ST_Intersection(geom, ST_Transform(BuildMBR(" ;
        sql << bbox.minx_-buffer << ',' << bbox.miny_-buffer << ',' << bbox.maxx_+buffer << ',' << bbox.maxy_+buffer << "," << 3857 ;
        sql << "),4326)))" ;
    }
    else sql << "geom" ;

    if ( tol != 0 )
        sql << ", " << tol << ")" ;

    sql << " AS _geom_ FROM " << tableName << " AS g JOIN kv ON kv.osm_id = g.osm_id";

    sql << " WHERE " ;
    sql << "(( kv.zoom_min BETWEEN " << (int)min_zoom << " AND " << max_zoom << " ) OR ( kv.zoom_max BETWEEN " << min_zoom << " AND " << max_zoom << " ) OR ( kv.zoom_min <= " << min_zoom << " AND kv.zoom_max >= " << max_zoom << "))" ;

    sql << "AND g.ROWID IN ( SELECT ROWID FROM SpatialIndex WHERE f_table_name='" << tableName << "' AND search_frame = ST_Transform(BuildMBR(" ;
    sql << bbox.minx_-buffer << ',' << bbox.miny_-buffer << ',' << bbox.maxx_+buffer << ',' << bbox.maxy_+buffer << "," << 3857 << "),4326)) AND _geom_ NOT NULL" ;

    return sql.str() ;
}

static bool fetch_pois(SQLite::Database &db, const BBox &bbox, uint8_t min_zoom, uint8_t max_zoom, vector<POI> &pois, vector<vector<uint32_t>> &pois_per_level) {
    try {
        SQLite::Session session(&db) ;
        SQLite::Connection &con = session.handle() ;

        string sql = make_bbox_query("geom_pois", bbox, min_zoom, max_zoom, false, 0, 0) ;
        SQLite::Query q(con, sql) ;

        SQLite::QueryResult res = q.exec() ;

        string prev_id ;

        while ( res ) { // each geometry appears as many times as the number of associated tags, here we group the results

            string osm_id = res.get<string>(0) ;
            string key = res.get<string>(1) ;
            string val = res.get<string>(2) ;
            uint8_t minz = res.get<int>(3) ;
            uint8_t maxz = res.get<int>(4) ;

            if ( osm_id != prev_id ) {
                int blob_size ;
                const char *data = res.getBlob(5, blob_size) ;

                gaiaGeomCollPtr geom = gaiaFromSpatiaLiteBlobWkb ((const unsigned char *)data, blob_size);

                for( gaiaPointPtr p = geom->FirstPoint ; p != nullptr ; p = p->Next ) {
                    double lon = p->X ;
                    double lat = p->Y ;

                    pois.emplace_back(lat, lon) ;

                    // we add the poi at the lowest possible level
                    int z = std::max<int>(minz, min_zoom) - (int)min_zoom;
                    pois_per_level[z].push_back(pois.size()-1) ;
                }

                pois.back().tags_.add(key, val) ;
            }

            prev_id = osm_id ;

            res.next() ;
        }

        return true ;

    }
    catch ( SQLite::Exception &e) {
        cerr << e.what() << endl ;
        return false ;
    }
}


static bool fetch_lines(const std::string &tableName, SQLite::Database &db, const BBox &bbox, uint8_t min_zoom, uint8_t max_zoom,
                        bool clip, double buffer, double tol,
                        vector<WayDataContainer> &ways, vector<vector<uint32_t>> &ways_per_level) {
    try {
        SQLite::Session session(&db) ;
        SQLite::Connection &con = session.handle() ;

        string sql = make_bbox_query(tableName, bbox, min_zoom, max_zoom, clip, buffer, tol) ;
        SQLite::Query q(con, sql) ;

        SQLite::QueryResult res = q.exec() ;

        string prev_id ;

        while ( res ) { // each geometry appears as many times as the number of associated tags, here we group the results

            string osm_id = res.get<string>(0) ;
            string key = res.get<string>(1) ;
            string val = res.get<string>(2) ;
            uint8_t minz = res.get<int>(3) ;
            uint8_t maxz = res.get<int>(4) ;

            if ( osm_id != prev_id ) {
                int blob_size ;
                const char *data = res.getBlob(5, blob_size) ;

                gaiaGeomCollPtr geom = gaiaFromSpatiaLiteBlobWkb ((const unsigned char *)data, blob_size);

                // each line may be broken into multiple linestrings by clipping
                WayDataContainer wc ;

                for( gaiaLinestringPtr p = geom->FirstLinestring ; p != nullptr ; p = p->Next ) {
                    double *coords = p->Coords ;
                    WayDataBlock block ;
                    block.coords_.resize(1) ;

                    for( uint i=0 ; i<p->Points ; i++ ) {
                        double lon = *coords++ ;
                        double lat = *coords++ ;
                        block.coords_[0].emplace_back(lat, lon) ;
                    }

                    wc.blocks_.emplace_back(block) ;
                }

                ways.emplace_back(wc) ;

                // we add the way at the lowest possible level
                int z = std::max<int>(minz, min_zoom) - (int)min_zoom;
                ways_per_level[z].push_back(ways.size()-1) ;
            }

            ways.back().tags_.add(key, val) ;

            prev_id = osm_id ;

            res.next() ;
        }

        return true ;

    }
    catch ( SQLite::Exception &e) {
        cerr << e.what() << endl ;
        return false ;
    }
}

static bool fetch_polygons(SQLite::Database &db, const BBox &bbox, uint8_t min_zoom, uint8_t max_zoom, bool clip,
                           double buffer, double tol, vector<WayDataContainer> &ways, vector<vector<uint32_t>> &ways_per_level) {
    try {
        SQLite::Session session(&db) ;
        SQLite::Connection &con = session.handle() ;

        string sql = make_bbox_query("geom_polygons", bbox, min_zoom, max_zoom, clip, buffer, tol) ;
        SQLite::Query q(con, sql) ;

        SQLite::QueryResult res = q.exec() ;

        string prev_id ;

        while ( res ) { // each geometry appears as many times as the number of associated tags, here we group the results

            string osm_id = res.get<string>(0) ;
            string key = res.get<string>(1) ;
            string val = res.get<string>(2) ;
            uint8_t minz = res.get<int>(3) ;
            uint8_t maxz = res.get<int>(4) ;

            if ( osm_id != prev_id ) {
                int blob_size ;
                const char *data = res.getBlob(5, blob_size) ;

                gaiaGeomCollPtr geom = gaiaFromSpatiaLiteBlobWkb ((const unsigned char *)data, blob_size);

                // each polygon may be broken into multiple polygons by clipping
                WayDataContainer wc ;

                for( gaiaPolygonPtr p = geom->FirstPolygon ; p != nullptr ; p = p->Next ) {

                    uint n_interior_rings = p->NumInteriors ;

                    WayDataBlock block ;
                    block.coords_.resize(1 + n_interior_rings) ;

                    gaiaRingPtr ex_ring = p->Exterior ;
                    double *coords = ex_ring->Coords ;

                    for( uint i=0 ; i<ex_ring->Points ; i++ ) {
                        double lon = *coords++ ;
                        double lat = *coords++ ;
                        block.coords_[0].emplace_back(lat, lon) ;
                    }

                    uint k=1 ;
                    for( uint k=0 ; k< n_interior_rings ; k++ ) {
                        gaiaRing &ir_ring = p->Interiors[k] ;
                        double *coords = ir_ring.Coords ;

                        for( uint i=0 ; i<ir_ring.Points ; i++ ) {
                            double lon = *coords++ ;
                            double lat = *coords++ ;
                            block.coords_[k+1].emplace_back(lat, lon) ;
                        }
                    }

                    wc.blocks_.emplace_back(block) ;
                }

                ways.emplace_back(wc) ;

                // we add the way at the lowest possible level
                int z = std::max<int>(minz, min_zoom) - (int)min_zoom;
                ways_per_level[z].push_back(ways.size()-1) ;
            }

            ways.back().tags_.add(key, val) ;


            prev_id = osm_id ;

            res.next() ;
        }

        return true ;

    }
    catch ( SQLite::Exception &e) {
        cerr << e.what() << endl ;
        return false ;
    }
}

// Computes the amount of latitude degrees for a given distance in pixel at a given zoom level.
static double deltaLat(double delta, double lat, uint8_t zoom) {
   double mx, my, dlat, lon, px, py ;
    tms::latlonToMeters(lat, 0, mx, my) ;
    tms::metersToPixels(mx, my, zoom, px, py) ;

    py += delta ;
    tms::pixelsToMeters(px, py, zoom, mx, my) ;
    tms::metersToLatLon(mx, my, dlat, lon) ;

    return fabs(dlat - lat) ;
}

uint64_t MapFile::writeTileData(int32_t tx, int32_t ty, int32_t tz, uint8_t min_zoom, uint8_t max_zoom, SQLite::Database &db, const WriteOptions &options)
{
    BBox bbox ;
    TileKey bt(tx, ty, tz, true) ;
    tms::tileBounds(bt.x(), bt.y(), bt.z(), bbox.minx_, bbox.miny_, bbox.maxx_, bbox.maxy_) ;
    int nz = (int)max_zoom - (int)min_zoom + 1 ;

    vector<POI> pois ;
    vector<vector<uint32_t>> pois_per_level(nz) ;

    fetch_pois(db, bbox, min_zoom, max_zoom, pois, pois_per_level) ;

    vector<WayDataContainer> ways ;
    vector<vector<uint32_t>> ways_per_level(nz) ;

    // we compute simplification factor per subfile
    double tol = ( tz <= 12 ) ? deltaLat(options.simplification_factor_, info_.max_lat_, max_zoom) : 0 ;

    fetch_lines("geom_lines", db, bbox, min_zoom, max_zoom, options.way_clipping_, options.bbox_enlargement_, tol, ways, ways_per_level) ;
    fetch_lines("geom_relations", db, bbox, min_zoom, max_zoom, options.way_clipping_, options.bbox_enlargement_,tol,  ways, ways_per_level) ;
    fetch_polygons(db, bbox, min_zoom, max_zoom, options.way_clipping_, options.bbox_enlargement_, tol, ways, ways_per_level) ;

    double min_lat, min_lon, max_lat, max_lon ;
    tms::tileLatLonBounds(bt.x(), bt.y(), bt.z(), min_lat, min_lon, max_lat, max_lon) ;

    MapFileOSerializer s(strm_) ;

    // write header
    if ( options.debug_ ) {
        stringstream sigstrm ;
        sigstrm << "###TileStart" << tx << ',' << ty << "###" ;
        int extra = 32-sigstrm.tellp() ;
        for ( int i=0 ; i<extra ; i++ ) sigstrm.put(' ') ;
        s.write_bytes((uint8_t *)sigstrm.str().c_str(), 32) ;
    }

    // write POI and way number
    for ( uint i=0 ; i<nz ; i++ ) {
        s.write_var_uint64(pois_per_level[i].size()) ;
        s.write_var_uint64(ways_per_level[i].size()) ;
    }

    // we write POI and way data into a memory buffer since we need to find the offsets

    string poi_data = writePOIData(pois, pois_per_level, max_lat, min_lon) ;
    string way_data = writeWayData(ways, ways_per_level, max_lat, min_lon) ;

    s.write_var_uint64(poi_data.size()) ;
    s.write_bytes((uint8_t *)&poi_data[0], poi_data.size()) ;
    s.write_bytes((uint8_t *)&way_data[0], way_data.size()) ;

}

string MapFile::writePOIData(const vector<POI> &pois, const vector<vector<uint32_t> > &pois_per_level, double lat0, double lon0)
{
    ostringstream strm ;
    MapFileOSerializer buffer(strm) ;

    uint nz = pois_per_level.size() ;

    for( uint i=0 ; i<pois_per_level.size() ; i++ ) {

        for( uint j=0 ; j<pois_per_level[i].size() ; j++ ) {
            uint32_t idx = pois_per_level[i][j] ;
            const POI &poi = pois[idx] ;

            if ( has_debug_info_ ) {
        // write header
            stringstream sigstrm ;
            sigstrm << "###POIStart" << poi.id_ << "###" ;
            int extra = 32-sigstrm.tellp() ;
            for ( int i=0 ; i<extra ; i++ ) sigstrm.put(' ') ;
            buffer.write_bytes((uint8_t *)sigstrm.str().c_str(), 32) ;
        }

            buffer.write_var_int64(round((poi.lat_ - lat0)*1.0e6)) ;
            buffer.write_var_int64(round((poi.lon_ - lon0)*1.0e6)) ;

            uint8_t tflag = 0 ;

            uint n_tags = poi.tags_.count() ;

            tflag |= n_tags & 0x0f ;
            int8_t layer = stoi(poi.tags_.get("layer", "0") ) + 5 ;
            tflag |= ( layer << 4 ) & 0xf0;

            buffer.write_uint8(tflag) ;

/*
            for( uint i=0 ; i<ntags ; i++ ) {
                uint64_t tag_index = s.read_var_uint64() ;

                string kv = poi_tags_[tag_index], tag, val ;
                decode_key_value(kv, tag, val);

                poi.tags_.add(tag, val) ;
            }

    uint8_t cflag = s.read_uint8() ;

    if ( cflag & 0x80 )
        poi.tags_.add("name", s.read_utf8()) ;

    if ( cflag & 0x40 )
        poi.tags_.add("addr:housenumber", s.read_utf8()) ;

    if ( cflag & 0x20 )
        poi.tags_.add("ele", std::to_string(s.read_var_int64())) ;

    if ( layer != 0 )
        poi.tags_.add("layer", std::to_string(layer)) ;

    bytes += poi.tags_.capacity() ;

    return bytes ;*/
        }
    }

    return strm.str() ;

}

string MapFile::writeWayData(const vector<WayDataContainer> &ways, const vector<vector<uint32_t> > &ways_per_level, double lat, double lon)
{

}