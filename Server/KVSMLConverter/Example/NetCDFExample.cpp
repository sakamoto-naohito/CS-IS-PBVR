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
#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
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
            << "  4: MPASReader (using direct NetCDF reader)\n"
            << "  5: vtkNetCDFUGRIDReader\n"
            << "  6: vtkSLACReader\n"
            << "Input [1-6]: ";

        int selection = 0;

        if ( std::cin >> selection )
        {
            if ( selection >= 1 && selection <= 6 )
            {
                // 後続の getline が選択番号の入力行末を読み取らないようにする。
                std::cin.ignore( std::numeric_limits<std::streamsize>::max(), '\n' );

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
        }

        std::cerr << "Invalid input. Please enter a number from 1 to 6.\n\n";

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

int GetLayerThickness()
{
    constexpr int default_layer_thickness = 10000;

    while ( true )
    {
        std::cout << "Input MPAS layer thickness [1-200000] (default: 10000): ";

        std::string input;
        if ( !std::getline( std::cin, input ) )
        {
            throw std::runtime_error( "Input ended while reading MPAS layer thickness." );
        }

        const std::string::size_type first = input.find_first_not_of( " \t\r\n" );
        if ( first == std::string::npos )
        {
            return default_layer_thickness;
        }
        const std::string::size_type last = input.find_last_not_of( " \t\r\n" );
        const std::string value_text = input.substr( first, last - first + 1 );

        std::istringstream parser( value_text );
        long long value = 0;
        std::string extra;
        if ( ( parser >> value ) && !( parser >> extra ) && value >= 1 && value <= 200000 )
        {
            return static_cast<int>( value );
        }

        std::cerr << "Invalid input. Please enter an integer from 1 to 200000.\n";
    }
}

bool GetIsAtmosphere()
{
    while ( true )
    {
        std::cout << "Is this MPAS atmosphere data? [true/false or 1/0] (default: false): ";

        std::string input;
        if ( !std::getline( std::cin, input ) )
        {
            throw std::runtime_error( "Input ended while reading MPAS atmosphere setting." );
        }

        const std::string::size_type first = input.find_first_not_of( " \t\r\n" );
        if ( first == std::string::npos )
        {
            return false;
        }
        const std::string::size_type last = input.find_last_not_of( " \t\r\n" );
        std::string value = input.substr( first, last - first + 1 );
        std::transform(
            value.begin(), value.end(), value.begin(), []( unsigned char character )
            {
                return static_cast<char>( std::tolower( character ) );
            } );

        if ( value == "true" || value == "1" )
        {
            return true;
        }
        if ( value == "false" || value == "0" )
        {
            return false;
        }

        std::cerr << "Invalid input. Please enter true, false, 1, or 0.\n";
    }
}

void SeriesNetCDFSLAC2KVSML( const std::string& directory, const std::string& base, const std::string& src, const std::string& mode_file_path )
{
    std::unordered_map<int, cvt::UnstructuredPfi> pfi_map;
    cvt::NetCDF::ReaderType reader_type = cvt::NetCDF::ReaderType::SLAC;
    int layer_thickness = cvt::NetCDF::DefaultMPASLayerThickness; // for MPAS
    bool is_atmosphere = cvt::NetCDF::DefaultMPASIsAtmosphere; // for MPAS

    cvt::NumeralSequenceFileNames sequence( mode_file_path );

    int last_time_step = sequence.numberOfFiles() - 1;
    int time_step = 0;
    int sub_volume_id = 1;
    int sub_volume_count = 1;

    for ( const auto& filename : sequence.fileNames() )
    {
        std::cout << "Reading " << filename << " ..." << std::endl;
        cvt::NetCDF netcdf( src, reader_type, filename, layer_thickness, is_atmosphere );

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

    return;
}

void NetCDF2Kvsml( const std::string& directory, const std::string& base, const std::string& src )
{

    std::cout << "reading " << src << " ..." << std::endl;

    cvt::NetCDF::ReaderType reader_type;
    std::string sub_file_path;
    int layer_thickness = cvt::NetCDF::DefaultMPASLayerThickness; // for MPAS
    bool is_atmosphere = cvt::NetCDF::DefaultMPASIsAtmosphere; // for MPAS

    reader_type = SelectNetCDFReader();

    if ( reader_type == cvt::NetCDF::ReaderType::NetCDFCAM )
    {
        sub_file_path = GetConnectivityFilePath();
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::NetCDFMPAS )
    {
        layer_thickness = GetLayerThickness();
        is_atmosphere = GetIsAtmosphere();
        sub_file_path = "";
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::SLAC )
    {
        sub_file_path = GetModeFilePath();
    }
    else
    {
        sub_file_path = "";
    }

    // 補助ファイルパスにワイルドカードが含まれている場合
    if ( sub_file_path.find_first_of( "*" ) != std::string::npos )
    {
        if ( reader_type == cvt::NetCDF::ReaderType::SLAC )
        {
            // SLACは単一メッシュと連番modeファイルの組み合わせを扱う。
            SeriesNetCDFSLAC2KVSML( directory, base, src, sub_file_path );
            return;
        }
        else if ( reader_type == cvt::NetCDF::ReaderType::NetCDFCAM )
        {
            std::cerr << "The connectivity file does not support wildcards."
                      << "vtkNetCDFCAMReader: " << sub_file_path << std::endl;
            return;
        }
        else
        {
            std::cerr << "Unexpected wildcard in the sub file path. "
                      << "File: " << __FILE__ << ", function: " << __func__
                      << ", line: " << __LINE__ << std::endl;
            return;
        }
    }
    else
    {
        cvt::NetCDF input_netcdf( src, reader_type, sub_file_path, layer_thickness, is_atmosphere );

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
    }

    return;
}

void SeriesNetCDF2Kvsml( const std::string& directory, const std::string& base, const std::string& src )
{
    std::unordered_map<int, cvt::UnstructuredPfi> pfi_map;

    cvt::NetCDF::ReaderType reader_type;
    std::string sub_file_path;
    int layer_thickness = cvt::NetCDF::DefaultMPASLayerThickness; // for MPAS
    bool is_atmosphere = cvt::NetCDF::DefaultMPASIsAtmosphere; // for MPAS

    reader_type = SelectNetCDFReader();

    if ( reader_type == cvt::NetCDF::ReaderType::NetCDFCAM )
    {
        sub_file_path = GetConnectivityFilePath();
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::NetCDFMPAS )
    {
        layer_thickness = GetLayerThickness();
        is_atmosphere = GetIsAtmosphere();
    }
    else if ( reader_type == cvt::NetCDF::ReaderType::SLAC )
    {
        std::cerr << "Wildcards cannot be used in the SLAC mesh file. " << std::endl;
        std::cerr << "Wildcards can be used in the SLAC mode file. "    << std::endl;
        std::cerr << "SLAC mesh file: " << src                          << std::endl;
        std::cerr << "SLAC mode file: " << sub_file_path                << std::endl;
        return;
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
        cvt::NetCDF netcdf( filename, reader_type, sub_file_path, layer_thickness, is_atmosphere );

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
