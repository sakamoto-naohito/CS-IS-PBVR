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
#include "TimeSeriesFiles/NumeralSequenceFileNames.h"
#include "PBVRFileInformation/Pfl.h"

cvt::NetCDF::ReaderType SelectNetCDFReader()
{
    while ( true )
    {
        std::cout
            << "Select NetCDF reader:\n"
            << "  1: vtkNetCDFCFReader\n"
            << "  2: vtkNetCDFPOPReader\n"
            << "  3: vtkNetCDFCAMReader\n"
            << "  4: vtkMPASReader\n"
            << "  5: vtkNetCDFUGRIDReader\n"
            << "  6: vtkSLACReader\n"
            << "Input [1-6]: ";

        int selection = 0;

        if ( std::cin >> selection )
        {
            switch ( selection )
            {
            case 1:
                return cvt::NetCDF::ReaderType::NetCDFCF;
            case 2:
                return cvt::NetCDF::ReaderType::NetCDFPOP;
            case 3:
                return cvt::NetCDF::ReaderType::NetCDFCAM;
            case 4:
                return cvt::NetCDF::ReaderType::NetCDFMPAS;
            case 5:
                return cvt::NetCDF::ReaderType::NetCDFUGRID;
            case 6:
                return cvt::NetCDF::ReaderType::SLAC;
            }
        }

        std::cerr << "Invalid input. Please enter a number from 1 to 7.\n\n";

        // std::cin が "abc" などで fail 状態になった場合に復旧する。
        std::cin.clear();

        // 入力行の残りを捨てる。
        std::cin.ignore( std::numeric_limits<std::streamsize>::max(), '\n' );
    }
}

std::string GetConnectivityFilePath()
{
    std::cout << "Input connectivity file path: ";

    std::string file_path;
    std::getline( std::cin >> std::ws, file_path );
    return file_path;
}

std::string GetModeFilePath()
{
    std::cout << "Input mode file path: ";

    std::string file_path;
    std::getline( std::cin >> std::ws, file_path );
    return file_path;
}

void NetCDF2Kvsml( const std::string& directory, const std::string& base, const std::string& src )
{

    std::cout << "reading " << src << " ..." << std::endl;

    cvt::NetCDF::ReaderType reader_type;
    std::string sub_file_path;

    reader_type = SelectNetCDFReader();

    if ( reader_type == cvt::NetCDF::ReaderType::NetCDFCAM )
    {
        sub_file_path = GetConnectivityFilePath();
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::SLAC )
    {
        sub_file_path = GetModeFilePath();
    }
    else
    {
        sub_file_path = "";
    }

    cvt::NetCDF input_netcdf( src, reader_type, sub_file_path );

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

    cvt::NetCDF::ReaderType reader_type;
    std::string sub_file_path;

    reader_type = SelectNetCDFReader();

    if ( reader_type == cvt::NetCDF::ReaderType::NetCDFCAM )
    {
        sub_file_path = GetConnectivityFilePath();
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::SLAC )
    {
        sub_file_path = GetModeFilePath();
    }
    else
    {
        sub_file_path = "";
    }
    
    cvt::NumeralSequenceFileNames sequence( src );

    int last_time_step = sequence.numberOfFiles() - 1;
    int time_step = 0;
    int sub_volume_id = 1;
    int sub_volume_count = 1;

    for ( const auto& filename : sequence.fileNames() )
    {
        std::cout << "Reading " << filename << " ..." << std::endl;
        cvt::NetCDF netcdf( filename, reader_type, sub_file_path );

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
