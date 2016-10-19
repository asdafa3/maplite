#include "map_overlay_manager.hpp"
#include "map_overlay.hpp"
#include "map_widget.hpp"

#include "spatialindex/SpatialIndex.h"


#include <QFileInfo>
#include <QDataStream>
#include <QDebug>
#include <QSettings>

#include <float.h>

#include "database.hpp"

using namespace SpatialIndex ;
using namespace std ;

const size_t MapOverlayManager::index_page_size_ = 4096 ;
const size_t MapOverlayManager::index_buffer_capacity_ = 256 ;

bool MapOverlayManager::open(const QString &storage)
{
    // this is the sqlite database that contains all overlay data indexed by ID.

    QString db_path = storage + ".sqlite" ;


    bool populate_db = !QFileInfo(db_path).exists() ;
    db_ = new SQLite::Database((const char *)db_path.toUtf8()) ;

    if ( populate_db ) { // create database for the first time

        SQLite::Session session(db_) ;
        SQLite::Connection &con = session.handle() ;

        con.exec("CREATE TABLE overlays (id INTEGER PRIMARY KEY AUTOINCREMENT, type TEXT NOT NULL, name TEXT NOT NULL, content BLOB NOT NULL);\
CREATE TABLE collections (id INTEGER PRIMARY KEY AUTOINCREMENT, folder_id INTEGER NOT NULL, is_visible INTEGER DEFAULT 1, name TEXT, attributes BLOB);\
CREATE TABLE memberships (overlay_id INTEGER, collection_id INTEGER);\
CREATE INDEX membership_idx ON memberships (overlay_id);\
CREATE UNIQUE INDEX membership_unique_idx ON memberships(overlay_id, collection_id); \
CREATE TABLE folders (id INTEGER PRIMARY KEY AUTOINCREMENT, is_visible INTEGER DEFAULT 1, name TEXT NOT NULL, parent_id INTEGER NOT NULL);\
INSERT INTO folders (name, parent_id) VALUES ('Library', 0);\
PRAGMA journal_mode=WAL;PRAGMA synchronous=0") ;
    }

    // Now create the spatial index that uses a paged memory file

    id_type indexIdentifier;
    string index_path_prefix((const char *)storage.toUtf8()) ;

    if ( QFileInfo(storage + ".idx").exists() ) // load existing a spatial index saved on disk
    {
        try {
            storage_ = StorageManager::loadDiskStorageManager(index_path_prefix);
            buffer_ = StorageManager::createNewRandomEvictionsBuffer(*storage_, index_buffer_capacity_, false);

            QSettings sts ;
            indexIdentifier = sts.value("overlays/spatial_index_id", 1).toInt() ;

            index_ = RTree::loadRTree(*storage_, indexIdentifier);

            return true ;
        }
        catch (Tools::IllegalStateException &e )
        {
            // We probably are here due to corrupted index
            cout << "ok here" << endl ;
        }
     }

    // index not exists or corrupted, so create a new one
     storage_ = StorageManager::createNewDiskStorageManager(index_path_prefix, index_page_size_);
     buffer_ = StorageManager::createNewRandomEvictionsBuffer(*storage_, index_buffer_capacity_, false);
     index_ = RTree::createNewRTree(*buffer_, 0.7, 100, 100, 2, SpatialIndex::RTree::RV_RSTAR, indexIdentifier);

     QSettings sts ;
     sts.setValue("overlays/spatial_index_id", QVariant::fromValue<quint64>(indexIdentifier)) ;
     return true ;
}


class QueryVisitor : public IVisitor
{
public:
    QueryVisitor(QVector<quint64> &ovr): ovr_(ovr) {}

    void visitNode(const INode& n) {}

    void visitData(const IData& d)
    {
        ovr_.append(d.getIdentifier()) ;

        //cout << d.getIdentifier() << ' ' << (const char *)data << endl ;
    }

    void visitData(std::vector<const IData*>& v) {}

    QVector<quint64> &ovr_ ;
};

MapOverlayPtr MapOverlayManager::findNearest(const QByteArray &searchType, const QPoint &click, MapWidget *view, double thresh)
{
    QVector<quint64> ids ;

    QueryVisitor v(ids) ;

    QPointF coords = view->displayToCoords(click) ;

    double pts[2] ;
    pts[0] = coords.x() ;
    pts[1] = coords.y() ;
    SpatialIndex::Point p(pts, 2) ;

    index_->pointLocationQuery(p, v) ;

    if ( ids.isEmpty() ) return MapOverlayPtr() ;

    QVector<MapOverlayPtr> overlays ;

    getAllOverlays(ids, overlays) ;

    double min_dist = DBL_MAX ;
    MapOverlayPtr best ;

    Q_FOREACH(MapOverlayPtr overlay, overlays)
    {
        if ( searchType != "*" && overlay->type() != searchType )  continue ;

        int seg ;
        double dist = overlay->distanceToPt(click, seg, view) ;

   //     qDebug() << overlay->id() << dist ;

        if ( dist < min_dist )
        {
            min_dist = dist ;
            best = overlay ;
        }
    }

    if ( min_dist > thresh ) best = MapOverlayPtr() ;

    return best ;
}

MapOverlayPtr MapOverlayManager::load(quint64 id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Query q(con, "select f.id as id, f.type as type, f.name as name, f.content as content, c.is_visible as is_visible from overlays as f join memberships as m on m.overlay_id = f.id join collections as c on c.id = collection_id where f.id = ?;") ;

        q.bind((long long int)id) ;

        SQLite::QueryResult res = q.exec() ;

        if ( res )
        {
            string type = res.get<string>(1) ;
            QString name = QString::fromUtf8(res.get<string>(2).c_str()) ;

            MapOverlayPtr obj = MapOverlay::create(type, name) ;

            int bs ;
            const char *blob = res.getBlob(3, bs) ;

            QByteArray ba(blob, bs) ;

            obj->deserialize(ba);
            obj->storage_id_ = id ;

            bool is_visible = res.get<bool>(4) ;
            obj->visible_ = is_visible ;

            return obj ;
        }
        else return MapOverlayPtr();

    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return MapOverlayPtr() ;
    }
}

bool MapOverlayManager::update(const MapOverlayPtr &overlay)
{
    // we need to load the old overlay to get its MBR
    MapOverlayPtr old_overlay = load(overlay->id()) ;

    if (!old_overlay ) return false ;

    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Command q(con, "UPDATE overlays SET name=?, content=? WHERE id=?;") ;

        q.bind(overlay->name().toUtf8().data()) ;

        QByteArray ba = overlay->serialize() ;
        q.bind(ba.data(), ba.size()) ;
        q.bind((long long int)overlay->id()) ;

        q.exec() ;

        // delete old object from spatial index

        double plow[2], phigh[2];

        QRectF mbr = old_overlay->boundingBox().normalized() ;

        plow[0] = std::min(mbr.left(), mbr.right());
        plow[1] = std::min(mbr.top(), mbr.bottom());
        phigh[0] = std::max(mbr.left(), mbr.right());
        phigh[1] = std::max(mbr.top(), mbr.bottom()) ;

        SpatialIndex::Region r_old(plow, phigh, 2);

        index_->deleteData(r_old, overlay->id()) ;

        mbr = overlay->boundingBox().normalized() ;

        plow[0] = std::min(mbr.left(), mbr.right());
        plow[1] = std::min(mbr.top(), mbr.bottom());
        phigh[0] = std::max(mbr.left(), mbr.right());
        phigh[1] = std::max(mbr.top(), mbr.bottom()) ;

        SpatialIndex::Region r_new(plow, phigh, 2);

        std::ostringstream os;
        os << r_new;
        std::string idata = os.str();

        index_->insertData(idata.size() + 1, reinterpret_cast<const byte*>(idata.c_str()), r_new, overlay->id()) ;

        return true ;

    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}


bool MapOverlayManager::write(const QVector<MapOverlayPtr> &objects, quint64 collection_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        {
            // write overlay table

            SQLite::Transaction trans(con) ;

            SQLite::Command cmd(con, "INSERT INTO overlays (type, name, content) VALUES (?, ?, ?);") ;

            for(int i=0 ; i<objects.size() ; i++)
            {
                MapOverlayPtr object = objects[i] ;

                QByteArray data = object->serialize() ;
                QByteArray name = object->name().toUtf8() ;

                cmd.bind(1, object->type().data()) ;
                cmd.bind(2, name.data()) ;
                cmd.bind(3, (const void *)data.data(), data.size()) ;

                cmd.exec() ;

                sqlite3_int64 row_id = con.last_insert_rowid() ;
                object->storage_id_ = row_id ;

                cmd.clear() ;
            }

            trans.commit() ;
        }


        {
            // write membership table

            SQLite::Transaction trans(con) ;
            SQLite::Command cmd(con, "INSERT INTO memberships (overlay_id, collection_id) VALUES (?, ?)") ;

            for(int i=0 ; i<objects.size() ; i++)
            {
                MapOverlayPtr object = objects[i] ;

                cmd.bind(1, (long long int)object->storage_id_) ;
                cmd.bind(2, (long long int)collection_id) ;

                cmd.exec() ;
                cmd.clear() ;
            }

            trans.commit() ;
        }

        for(int i=0 ; i<objects.size() ; i++)
        {
            double plow[2], phigh[2];

            MapOverlayPtr object = objects[i] ;
            QRectF mbr = object->boundingBox().normalized() ;

            plow[0] = std::min(mbr.left(), mbr.right());
            plow[1] = std::min(mbr.top(), mbr.bottom());
            phigh[0] = std::max(mbr.left(), mbr.right());
            phigh[1] = std::max(mbr.top(), mbr.bottom()) ;

            SpatialIndex::Region r(plow, phigh, 2);

            std::ostringstream os;
            os << r;
            std::string idata = os.str();

            index_->insertData(idata.size() + 1, reinterpret_cast<const byte*>(idata.c_str()), r, object->storage_id_) ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e)
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}



void MapOverlayManager::query(QVector<quint64> &ovr, QVector<QRectF> &boxes)
{
    QueryVisitor vis(ovr) ;

    Q_FOREACH(QRectF mbr, boxes)
    {
        double plow[2], phigh[2];

        plow[0] = std::min(mbr.left(), mbr.right());
        plow[1] = std::min(mbr.top(), mbr.bottom());
        phigh[0] = std::max(mbr.left(), mbr.right());
        phigh[1] = std::max(mbr.top(), mbr.bottom()) ;

        SpatialIndex::Region r(plow, phigh, 2);

        index_->intersectsWithQuery(r, vis);

    }

}

bool MapOverlayManager::addNewFolder(const QString &name, quint64 parent_id, QString &name_unique, quint64 &item_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    int counter = 0 ;

    try {

        SQLite::Query q(con, "SELECT id FROM folders WHERE name = ? LIMIT 1;") ;

        name_unique = name ;

        while (1)
        {
            q.bind(1, name_unique.toUtf8().data()) ;
            SQLite::QueryResult res = q.exec() ;

            if ( res ) name_unique = name + QString(" (%1)").arg(++counter) ;
            else break ;

            q.clear() ;
        }

        SQLite::Command cmd(con, "INSERT INTO folders (name, parent_id) VALUES (?, ?)") ;
        cmd.bind(name_unique.toUtf8().data()) ;

        long long id = parent_id ;
        cmd.bind(id) ;
        cmd.exec() ;

        item_id = con.last_insert_rowid() ;

        return true ;

    }
    catch ( SQLite::Exception &e )
    {
        cout << e.what() << endl;
        return false ;
    }


}


bool MapOverlayManager::addNewCollection(const QString &name, quint64 folder_id, const QMap<QString, QVariant> &attributes, QString &name_unique, quint64 &item_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    int counter = 0 ;

    try {

        SQLite::Query q(con, "SELECT id FROM collections WHERE name = ? LIMIT 1;") ;

        name_unique = name ;

        while (1)
        {
            q.bind(1, name_unique.toUtf8().data()) ;
            SQLite::QueryResult res = q.exec() ;

            if ( res ) name_unique = name + QString(" (%1)").arg(++counter) ;
            else break ;

            q.clear() ;
        }

        string sql = "INSERT INTO collections (folder_id, name, attributes) VALUES (?, ?, ?)" ;

        SQLite::Command cmd(con, sql) ;

        cmd.bind((long long int)folder_id) ;
        cmd.bind(name_unique.toUtf8().data()) ;

        QByteArray ba ;
        QDataStream ds(&ba, QIODevice::WriteOnly) ;

        ds << attributes ;

        cmd.bind((const void *)ba.data(), ba.size()) ;
        cmd.exec() ;

        item_id = (quint64)con.last_insert_rowid() ;

        return true ;

    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }


}

bool MapOverlayManager::addOverlayInCollection(quint64 collection_id, const QVector<quint64> &overlays)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Transaction trans(con) ;

        SQLite::Command cmd(con, "REPLACE INTO memberships (overlay_id, collection_id) VALUES (?, ?);") ;

        Q_FOREACH(quint64 id, overlays)
        {
            cmd.bind(1, (long long)id) ;
            cmd.bind(2, (long long)collection_id) ;

            cmd.exec() ;
            cmd.clear() ;
        }

        trans.commit() ;

        return true ;

    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

bool MapOverlayManager::deleteOverlaysFromCollection(quint64 collection_id, const QVector<quint64> &overlays)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Transaction trans(con) ;

        SQLite::Command cmd(con, "DELETE FROM memberships WHERE overlay_id = ? AND collection_id = ?;") ;

        Q_FOREACH(quint64 id, overlays)
        {
            cmd.bind(1, (long long)id) ;
            cmd.bind(2, (long long)collection_id) ;

            cmd.exec() ;
            cmd.clear() ;
        }

        trans.commit() ;

        return true ;

    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}


MapOverlayManager::MapOverlayManager(): index_(0), db_(0) {
}

MapOverlayManager::~MapOverlayManager() {
   cleanup() ;
}


bool MapOverlayManager::getSubFolders(QVector<quint64> &ids, QVector<QString> &names, QVector<bool> &states, quint64 parent_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        SQLite::Query q(con, "SELECT id, name, is_visible FROM folders where parent_id=?;") ;
        quint64 id = parent_id ;
        q.bind(id) ;
        SQLite::QueryResult res = q.exec() ;

        while ( res )
        {
            quint64 id = res.get<long long int>(0) ;
            string name = res.get<string>(1) ;
            bool state = res.get<bool>(2) ;

            ids.push_back(id) ;
            names.push_back(QString::fromUtf8(name.c_str())) ;
            states.push_back(state) ;

            res.next() ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}


bool MapOverlayManager::getFolderCollections(QVector<quint64> &ids, QVector<QString> &names, QVector<bool> &states, quint64 folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        SQLite::Query q(con, "SELECT id, name, is_visible FROM collections where folder_id=?;") ;
        long long int id = folder_id ;
        q.bind(id) ;
        SQLite::QueryResult res = q.exec() ;

        while ( res )
        {
            quint64 id = res.get<long long int>(0) ;
            string name = res.get<string>(1) ;
            bool state = res.get<bool>(2) ;

            ids.push_back(id) ;
            names.push_back(QString::fromUtf8(name.c_str())) ;
            states.push_back(state) ;

            res.next() ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

int MapOverlayManager::getNumCollections(quint64 folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        SQLite::Query q(con, "SELECT count(*) FROM collections where folder_id=?;") ;
        long long int id = folder_id ;
        q.bind(id) ;
        SQLite::QueryResult res = q.exec() ;

        if ( res )
            return res.get<int>(0) ;
        else
            return 0 ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return 0 ;
    }

}

bool MapOverlayManager::getAllOverlays(const QVector<quint64> overlay_ids, QVector<MapOverlayPtr> &overlays)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    QByteArray idlist ;

    Q_FOREACH(quint64 id, overlay_ids)
    {
        if ( !idlist.isEmpty() ) idlist += ',' ;
        idlist += QString("%1").arg(id).toLatin1() ;
    }

    try {
        QByteArray sql = "SELECT * FROM overlays WHERE id IN (" ;
        sql += idlist + ')' ;

        SQLite::Query q(con, sql.data()) ;

        SQLite::QueryResult res = q.exec() ;

        while ( res )
        {
            quint64 id = res.get<long long int>(0) ;
            string type = res.get<string>(1) ;
            string name = res.get<string>(2) ;

            MapOverlayPtr overlay = MapOverlay::create(type, QString::fromUtf8(name.c_str())) ;
            overlay->storage_id_ = id ;

            int bs ;
            const char *blob = res.getBlob(3, bs) ;

            QByteArray ba(blob, bs) ;

            overlay->deserialize(ba);

            overlays.append(overlay) ;

            res.next() ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}


bool MapOverlayManager::getAllOverlaysInCollection(quint64 collection_id, QVector<MapOverlayPtr> &overlays)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        SQLite::Query q(con, "SELECT f.id, f.type, f.name, f.content FROM overlays AS f JOIN memberships AS m ON m.overlay_id = f.id WHERE m.collection_id = ?;") ;
        long long int id = collection_id ;
        q.bind(id) ;
        SQLite::QueryResult res = q.exec() ;

        while ( res )
        {
            quint64 id = res.get<long long int>(0) ;
            string type = res.get<string>(1) ;
            string name = res.get<string>(2) ;

            MapOverlayPtr overlay = MapOverlay::create(type, QString::fromUtf8(name.c_str())) ;
            overlay->storage_id_ = id ;

            int bs ;
            const char *blob = res.getBlob(3, bs) ;

            QByteArray ba(blob, bs) ;

            overlay->deserialize(ba);

            overlays.append(overlay) ;

            res.next() ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}


bool MapOverlayManager::deleteFolder(quint64 folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {
        // delete collectiions in this folder

        long long int id = folder_id ;

        {
            SQLite::Query q(con, "SELECT id FROM collections where folder_id=?;") ;

            q.bind(id) ;
            SQLite::QueryResult res = q.exec() ;

            while ( res )
            {
                quint64 id = res.get<long long int>(0) ;
                deleteCollection(id) ;
                res.next() ;
            }
        }
        // delete all child folders

        {
            SQLite::Query q(con, "SELECT id FROM folders where parent_id=?;") ;

            q.bind(id) ;

            SQLite::QueryResult res = q.exec() ;

            while ( res )
            {
                quint64 id = res.get<long long int>(0) ;
                deleteFolder(id) ;
                res.next() ;
            }
        }

        // remove the folder itself

        SQLite::Command cmd(con, "DELETE FROM folders where id=?;") ;

        cmd.bind(id) ;
        cmd.exec() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

bool MapOverlayManager::deleteCollection(quint64 collection_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Transaction trans(con) ;

        long long int id = collection_id ;

        SQLite::Command cmd1(con, "DELETE FROM collections where id=?;") ;

        cmd1.bind(id) ;
        cmd1.exec() ;

        SQLite::Command cmd2(con, "DELETE FROM overlays where id IN ( SELECT overlay_id FROM memberships WHERE collection_id=?);") ;

        cmd2.bind(id) ;
        cmd2.exec() ;

        SQLite::Command cmd3(con, "DELETE FROM memberships where collection_id=?;") ;

        cmd3.bind(id) ;
        cmd3.exec() ;

        trans.commit() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}


bool MapOverlayManager::getOverlayCollectionAndFolder(quint64 overlay_id, quint64 &collection_id, quint64 &folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        {
            SQLite::Query cmd(con, "SELECT collection_id from memberships WHERE overlay_id=? LIMIT 1;") ;

            cmd.bind(overlay_id) ;
            SQLite::QueryResult res = cmd.exec() ;

            if ( !res ) return false ;

            collection_id = res.get<quint64>(0) ;
        }

        {

            SQLite::Query cmd(con, "SELECT folder_id from collections WHERE id=? LIMIT 1;") ;

            cmd.bind(collection_id) ;
            SQLite::QueryResult res = cmd.exec() ;

            if ( !res ) return false ;

            folder_id = res.get<quint64>(0) ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}


bool MapOverlayManager::renameFolder(quint64 folder_id, const QString &newName)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        long long int id = folder_id ;

        SQLite::Command cmd(con, "UPDATE folders SET name=? WHERE id=?;") ;

        cmd.bind(newName.toUtf8().data()) ;
        cmd.bind(id) ;
        cmd.exec() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}


bool MapOverlayManager::renameCollection(quint64 collection_id, const QString &newName)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        long long int id = collection_id ;

        SQLite::Command cmd(con, "UPDATE collections SET name=? WHERE id=?;") ;

        cmd.bind(newName.toUtf8().data()) ;
        cmd.bind(id) ;
        cmd.exec() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

bool MapOverlayManager::moveFolder(quint64 folder_id, quint64 parent_folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        long long int id = folder_id ;
        long long int parent_id = parent_folder_id ;

        SQLite::Command cmd(con, "UPDATE folders SET parent_id=? WHERE id=?;") ;

        cmd.bind(parent_id) ;
        cmd.bind(id) ;
        cmd.exec() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}

bool MapOverlayManager::moveCollection(quint64 collection_id, quint64 parent_folder_id)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        long long int id = collection_id ;
        long long int parent_id = parent_folder_id ;

        SQLite::Command cmd(con, "UPDATE collections SET folder_id=? WHERE id=?;") ;

        cmd.bind(parent_id) ;
        cmd.bind(id) ;
        cmd.exec() ;

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }

}

bool MapOverlayManager::setCollectionVisibility(quint64 id, bool state)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        {
            SQLite::Command cmd(con, "UPDATE collections SET is_visible=? WHERE id=?;") ;

            cmd.bind(state) ;
            cmd.bind(id) ;
            cmd.exec() ;
        }

        if ( state ) // is checked state we have to check the parent folder too
        {

            SQLite::Query cmd(con, "SELECT folder_id FROM collections WHERE id=?;") ;

            cmd.bind(id) ;
            SQLite::QueryResult res = cmd.exec() ;

            setFolderVisibility(res.get<quint64>(0), state, false) ;
        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

bool MapOverlayManager::setFolderVisibility(quint64 id, bool state, bool update_children)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        // update collections in this folder
        if ( update_children ){
            SQLite::Command q(con, "UPDATE collections SET is_visible=? where folder_id=?;") ;
            q.bind(state) ;
            q.bind(id) ;
            q.exec() ;
        }

        //update subfolders

        if ( update_children ) {

             SQLite::Query q(con, "SELECT id FROM folders where parent_id=?;") ;
             q.bind(id) ;
             SQLite::QueryResult res = q.exec() ;

             while ( res )
             {
                 quint64 cid = res.get<quint64>(0) ;
                 setFolderVisibility(cid, state) ;
                 res.next() ;
             }

        }

        // update this folder
        {
            SQLite::Command q(con, "UPDATE folders SET is_visible=? where id=?;") ;
            q.bind(state) ;
            q.bind(id) ;
            q.exec() ;
        }

        if ( state ) // is checked state we have to check the parent folder too
        {
           while (1)
           {
                {
                   SQLite::Query q(con, "SELECT parent_id FROM folders where id=?;") ;
                   q.bind(id) ;
                   SQLite::QueryResult res = q.exec() ;

                   id = res.get<quint64>(0) ;
               }

               if ( id == 0 ) break ;

               {
                   SQLite::Command q(con, "UPDATE folders SET is_visible=1 where id=?;") ;
                   q.bind(id) ;
                   q.exec() ;
               }
           }

        }

        return true ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return false ;
    }
}

void MapOverlayManager::cleanup()
{
    if ( index_ )
    {
        delete index_;
        delete buffer_;
        delete storage_;
        index_ = nullptr ;
    }

    if ( db_ ) {
        delete db_ ;
        db_ = nullptr ;
    }
}

QString MapOverlayManager::uniqueOverlayName(const QString &pattern, quint64 collection_id, int &counter)
{
    SQLite::Session session(db_) ;
    SQLite::Connection &con = session.handle() ;

    try {

        SQLite::Query q(con, "SELECT f.id FROM overlays AS f JOIN memberships AS m ON m.overlay_id = f.id WHERE f.name = ? AND m.collection_id = ? LIMIT 1;") ;

        QString name_unique = pattern.arg((int)counter, 3, 10, QChar('0')) ;

        while (1)
        {
            ++counter ;

            q.bind(1, name_unique.toUtf8().data()) ;
            q.bind(2, (long long)collection_id) ;

            SQLite::QueryResult res = q.exec() ;

            if ( res ) name_unique = pattern.arg((int)counter, 3, 10, QChar('0')) ;
            else break ;

            q.clear() ;
        }

        return name_unique ;
    }
    catch ( SQLite::Exception &e )
    {
        qDebug() <<  e.what() ;
        return QString() ;
    }

}

QRectF MapOverlayManager::getOverlayBBox(const QVector<quint64> &overlay_ids)
{
    QVector<MapOverlayPtr> overlays ;

    getAllOverlays(overlay_ids, overlays) ;

    QRectF box ;

    Q_FOREACH(MapOverlayPtr overlay, overlays)  {
        box = box.united(overlay->boundingBox()) ;
    }

    return box ;
}



QRectF MapOverlayManager::getCollectionBBox(quint64 collection_id)
{
    QVector<MapOverlayPtr> overlays ;

    getAllOverlaysInCollection(collection_id, overlays) ;

    QRectF box ;

    Q_FOREACH(MapOverlayPtr overlay, overlays) {
        box = box.united(overlay->boundingBox()) ;
    }

    return box ;
}

