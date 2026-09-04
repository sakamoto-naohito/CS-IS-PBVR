/*
 * Created by Japan Atomic Energy Agency
 *
 * To the extent possible under law, the person who associated CC0 with
 * this file has waived all copyright and related or neighboring rights
 * to this file.
 *
 * You should have received a copy of the CC0 legal code along with this
 * work. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
 */
#include <iostream>
#include <string>
#include <unordered_map>

#include "kvs/Indent"
#include "kvs/UnstructuredVolumeObject"

#include "Exporter/UnstructuredVolumeObjectExporter.h"
#include "FileFormat/NetCDF/NetCDF.h"
#include "Importer/VtkImporter.h"
#include "PBVRFileInformation/UnstructuredPfi.h"
#include "TimeSeriesFiles/NumeralSequenceFiles.h"
#include "PBVRFileInformation/Pfl.h"

void NetCDF2Kvsml( const std::string& directory, const std::string& base, const std::string& src )
{
    std::cout << "reading " << src << " ..." << std::endl;
    cvt::NetCDF input_netcdf( src );

    int time_step = 0;
    int last_time_step = 0;
    int sub_volume_id = 1;
    int sub_volume_count = 1;

    cvt::VtkImporter<cvt::NetCDF> importer( &input_netcdf );
    std::cout << "  cell type: " << importer.cellType() << std::endl;

    kvs::UnstructuredVolumeObject* object = &importer;
    object->print( std::cout, kvs::Indent( 4 ) );

    std::cout << "  Writing to " << directory << " ..." << std::endl;
    auto local_base = std::string( base ) + "_" + std::to_string( object->cellType() );

    cvt::UnstructuredVolumeObjectExporter exporter( &importer );
    exporter.setWritingDataTypeToExternalBinary();
    exporter.write( directory, local_base, time_step, sub_volume_id, sub_volume_count );
    // or
    // exporter.write( "<directory>/<local_base>_00000_0000001_0000001.kvsml" );

    cvt::UnstructuredPfi pfi( object->veclen(), last_time_step, sub_volume_count );
    pfi.registerObject( &exporter, time_step, sub_volume_id );
    pfi.write( directory, local_base );
    // or
    // pfi.write( "<directory>/<local_base>.pfi" );

    pfi.print( std::cout, 2 );

    return;
}

void SeriesNetCDF2Kvsml( const std::string& directory, const std::string& base, const std::string& src )
{
    std::unordered_map<int, cvt::UnstructuredPfi> pfi_map;

    cvt::NumeralSequenceFiles<cvt::NetCDF> time_series( src );

    int last_time_step = time_series.numberOfFiles() - 1;
    int time_step = 0;
    int sub_volume_id = 1;
    int sub_volume_count = 1;

    for ( auto netcdf : time_series.eachTimeStep() )
    {
        std::cout << "Reading " << netcdf.filename() << " ..." << std::endl;

        cvt::VtkImporter<cvt::NetCDF> importer( &netcdf );
        std::cout << "  cell type: " << importer.cellType() << std::endl;

        kvs::UnstructuredVolumeObject* object = &importer;
        object->print( std::cout, kvs::Indent( 4 ) );

        std::cout << "  Writing to " << directory << " ..." << std::endl;
        auto local_base = std::string( base ) + "_" + std::to_string( object->cellType() );

        cvt::UnstructuredVolumeObjectExporter exporter( &importer );
        exporter.setWritingDataTypeToExternalBinary();
        exporter.write( directory, local_base, time_step, sub_volume_id, sub_volume_count );

        if ( time_step == 0 )
        {
            pfi_map.emplace(
                static_cast<int>( object->cellType() ),
                cvt::UnstructuredPfi( object->veclen(), last_time_step, sub_volume_count )
            );
        }

        pfi_map.at( static_cast<int>( object->cellType() ) ).registerObject( &exporter, time_step, sub_volume_id );

        time_step++;
    }

    cvt::Pfl pfl;
    for ( auto& e : pfi_map )
    {
        std::string local_base = std::string( base ) + "_" + std::to_string( e.first );
        e.second.write( directory, local_base );
        e.second.print( std::cout );

        pfl.registerPfi( directory, local_base );
    }
    pfl.write( directory, base );
}
