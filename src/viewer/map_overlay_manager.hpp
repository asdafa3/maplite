#ifndef __OVERLAY_INDEX_H__
#define __OVERLAY_INDEX_H__

#include <QObject>
#include <QPolygonF>
#include <QMap>

#include "database.hpp"
#include "map_overlay.hpp"

namespace SpatialIndex {
    class ISpatialIndex ;
    class IStorageManager ;

    namespace StorageManager {
        class IBuffer ;
    }
}

class MapOverlay ;
class MapWidget ;

class MapOverlayManager {


public:

    MapOverlayManager() ;
    ~MapOverlayManager();

    // open or create spatial index with given prefix
    bool open(const QString &storage) ;


    // find feature under click coordinates
    MapOverlayPtr findNearest(const QByteArray &searchType, const QPoint &coord, MapWidget *view, double thresh);

    // load and create an object with this id from database
    MapOverlayPtr load(quint64 id) ;

    // update feature
    bool update(const MapOverlayPtr &feature) ;

    // write object(s) to database and spatial index
    bool write(const QVector<MapOverlayPtr> &objects, quint64 collection_id = 0) ;

    // query objects within the collection of rectangles
    void query(QVector<quint64> &ovr, QVector<QRectF> &boxes) ;

    bool addNewFolder(const QString &name, quint64 parent_id, QString &unique_name, quint64 &item_id) ;
    bool addNewCollection(const QString &name, quint64 parent_id, const QMap<QString, QVariant> &attributes, QString &unique_name, quint64 &item_id) ;

    bool addOverlayInCollection(quint64 collection_id, const QVector<quint64> &overlays) ;
    bool deleteOverlaysFromCollection(quint64 collection_id, const QVector<quint64> &overlays) ;

    bool getSubFolders(QVector<quint64> &ids, QVector<QString> &names, QVector<bool> &state, quint64 parent_id);
    bool getFolderCollections(QVector<quint64> &ids, QVector<QString> &names, QVector<bool> &states, quint64 folder_id);
    int getNumCollections(quint64 folder_id);
    bool getOverlayCollectionAndFolder(quint64 overlay_id, quint64 &collection_id, quint64 &folder_id) ;

    bool getAllOverlaysInCollection(quint64 collection_id, QVector<MapOverlayPtr> &overlays) ;
    bool getAllOverlays(const QVector<quint64> overlay_ids, QVector<MapOverlayPtr> &overlays);
    bool getAllOverlaysInFolder(quint64 folder_id, QVector<MapOverlayPtr> &features) ;

    bool deleteFolder(quint64 id) ;
    bool deleteCollection(quint64 id) ;

    bool renameFolder(quint64 id, const QString &newName) ;
    bool renameCollection(quint64 id, const QString &newName) ;

    bool moveFolder(quint64 folder_id, quint64 parent_folder_id) ;
    bool moveCollection(quint64 collection_id, quint64 parent_folder_id) ;

    bool setCollectionVisibility(quint64 collection_id, bool state) ;

    // get a unique feature name for a collection using the specified arg pattern. counter should contain the initial numeric value and is update after the call

    QString uniqueOverlayName(const QString &pattern, quint64 collection_id, int &counter);

    QRectF getOverlayBBox(const QVector<quint64> &feature_ids) ;
    QRectF getCollectionBBox(quint64 collection_id);
    QRectF getFolderBBox(quint64 folder_id);

    bool setFolderVisibility(quint64 id, bool state, bool update_children = true);

    void cleanup() ;

protected:

    bool write(const QByteArray &data, const QByteArray &type, const QRectF &mbr, quint64 collection_id, quint64 &id);

    friend class MapFeatureCollection ;

    SQLite::Database *db() const { return db_ ; }

private:

    SpatialIndex::ISpatialIndex *index_ ;           // R-Tree index
    SpatialIndex::IStorageManager* storage_ ;       // data storage (only id's are stored here)
    SpatialIndex::StorageManager::IBuffer *buffer_ ;// memory buffer containing loaded pages
    SQLite::Database *db_ ;                         // the actual data are serialized here

    static const size_t index_page_size_ ;
    static const size_t index_buffer_capacity_ ;
};


#endif
