/*
 * Copyright (c) 2022 Japan Atomic Energy Agency
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef CVT__NETCDF_H_INCLUDE
#define CVT__NETCDF_H_INCLUDE
#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "DirectNetCDFMPASReader.h"
#include "kvs/FileFormatBase"
#include "kvs/Message"

#include <vtkInformation.h>
#include <vtkNetCDFCAMReader.h>
#include <vtkNetCDFCFReader.h>
#include <vtkNew.h>
#include <vtkSmartPointer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkStringArray.h>
#include <vtkUnstructuredGrid.h>

namespace cvt
{
/**
 * A NetCDF file IO.
 */
class NetCDF : public kvs::FileFormatBase
{
public:
    /**
     * A base class type.
     */
    using BaseClass = kvs::FileFormatBase;
    /**
     * An inner VTK data type.
     */
    using VtkDataType = vtkUnstructuredGrid;

    enum class ReaderType
    {
        NetCDFCF,
        NetCDFPOP,
        NetCDFCAM,
        NetCDFMPAS,
        NetCDFUGRID,
        SLAC
    };

    static constexpr int DefaultMPASLayerThickness = 10000;
    static constexpr bool DefaultMPASIsAtmosphere = false;

public:
    /**
     * Construct an IO.
     */
    NetCDF():
        BaseClass(),
        m_reader_type( ReaderType::NetCDFCF ),
        m_layer_thickness( DefaultMPASLayerThickness ),
        m_is_atmosphere( DefaultMPASIsAtmosphere )
    {
    }

    /**
     * Construct an IO.
     *
     * \param[in] filename A file name.
     * \param[in] reader_type A reader type.
     * \param[in] sub_file_path A sub file path (CAM or SLAC).
     * \param[in] layer_thickness for MPAS.
     * \param[in] is_atmosphere for MPAS.
     */
    NetCDF(
        const std::string& filename,
        ReaderType reader_type,
        const std::string& sub_file_path = "",
        int layer_thickness = 10000,
        bool is_atmosphere = false ):
        BaseClass(),
        m_reader_type( reader_type ),
        m_sub_file_path( sub_file_path ),
        m_layer_thickness( layer_thickness ),
        m_is_atmosphere( is_atmosphere )
    {
        BaseClass::setFilename( filename );
        this->read( filename );
    }
    /**
     * Construct an IO.
     *
     * \param[in] filename A file name.
     * \param[in] reader_type A reader type.
     * \param[in] sub_file_path A sub file path (CAM or SLAC).
     * \param[in] layer_thickness for MPAS.
     * \param[in] is_atmosphere for MPAS.
     */
    NetCDF(
        std::string&& filename,
        ReaderType reader_type,
        const std::string& sub_file_path = "",
        int layer_thickness = 10000,
        bool is_atmosphere = false ):
        BaseClass(),
        m_reader_type( reader_type ),
        m_sub_file_path( sub_file_path ),
        m_layer_thickness( layer_thickness ),
        m_is_atmosphere( is_atmosphere )
    {
        BaseClass::setFilename( filename );
        this->read( filename );
    }

public:
    bool read( const std::string& filename ) override
    {
        setSuccess( false );
        try
        {
            // vtkUnstructuredGrid取得
            vtkSmartPointer<vtkUnstructuredGrid> grid;
            
            switch ( m_reader_type )
            {
            case ReaderType::NetCDFCF:
                grid = readNetCDFCF( filename );
                break;
            case ReaderType::NetCDFCAM:
                grid = readNetCDFCAM( filename, m_sub_file_path );
                break;
            case ReaderType::NetCDFMPAS:
                grid = readNetCDFMPAS( filename );
                break;
            case ReaderType::NetCDFUGRID:
                break;
            case ReaderType::SLAC:
                break;
            }

            if ( !grid )
            {
                throw std::runtime_error( "Selected NetCDF reader did not produce vtkUnstructuredGrid." );
            }

            if ( grid->GetNumberOfPoints() == 0 || grid->GetNumberOfCells() == 0 )
            {
                throw std::runtime_error( "Selected NetCDF reader produced empty vtkUnstructuredGrid." );
            }

            std::cout << "Unstructured grid points : " << grid->GetNumberOfPoints() << std::endl;
            std::cout << "Unstructured grid cells  : " << grid->GetNumberOfCells()  << std::endl;

            vtk_data = grid;

            setSuccess( true );
            return true;
        }
        catch ( std::exception& e )
        {
            kvsMessageError( e.what() );
            return false;
        }
    }

    bool write( const std::string& filename ) override
    {
        setSuccess( false );
        kvsMessageError( "This function has not been implemented yet" );
        return false;
    };

public:
    /**
     * Get a VTK data.
     *
     * \return A VTK data.
     */
    vtkSmartPointer<VtkDataType> get() { return vtk_data; }
    /**
     * Get a VTK data.
     *
     * \return A VTK data.
     */
    vtkSmartPointer<VtkDataType> get() const { return vtk_data; }

private:
    vtkSmartPointer<vtkUnstructuredGrid> readNetCDFCF( const std::string& filename )
    {
        // NetCDFファイルを読み込む
        vtkNew<vtkNetCDFCFReader> reader;
        reader->SetFileName( filename.c_str() );
        reader->SphericalCoordinatesOff();

        // メタデータを読み込む。
        // これにより変数や次元情報を取得できるようになる。
        if ( !reader->UpdateMetaData() )
        {
            throw std::runtime_error( "Failed to read NetCDF metadata: " + filename );
        }

        /*  
            // 緯度・経度座標は現在サポートしない
            // VTK-9.3.1ではGetLatitudeDimensionName(), GetLongitudeDimensionName()がprotectなので判定しない
            // 最新版ではpublicになっている
            const char* latitude_dimension  = reader->GetLatitudeDimensionName();
            const char* longitude_dimension = reader->GetLongitudeDimensionName();
            
            if ( ( latitude_dimension  && latitude_dimension[0]  != '\0' ) ||
            ( longitude_dimension && longitude_dimension[0] != '\0' ) )
            {
                throw std::runtime_error( "Latitude/longitude coordinates are not supported: " + filename );
            }
        */

        // time dimensionsを除いたdimensionsを取得
        vtkStringArray* variable_dimensions = reader->GetVariableDimensions();

        if ( !variable_dimensions )
        {
            throw std::runtime_error( "No dimensions found in NetCDF file: " + filename );
        }

        const int number_of_variables = reader->GetNumberOfVariableArrays();
        std::optional<std::string> volume_dimensions; // 検出した3次元dimensions

        // 3次元dimensionsを自動検出する
        for ( int i = 0; i < number_of_variables; ++i )
        {
            const std::string dimensions = variable_dimensions->GetValue( i );

            // 次元数の確認
            int number_of_dimensions = 0;

            if ( dimensions == "()" || dimensions.size() < 2 )
            {
                number_of_dimensions = 0;
            }
            else
            {
                number_of_dimensions = static_cast<int>( std::count( dimensions.begin(), dimensions.end(), ',' ) ) + 1;
            }

            if ( number_of_dimensions == 3 )
            {
                volume_dimensions = dimensions;
                break;
            }
        }

        if ( !volume_dimensions )
        {
            throw std::runtime_error( "No three-dimensional data variable was found: " + filename );
        }

        std::cout << "volume dimensions: " << *volume_dimensions << std::endl;

        // 検出した3次元dimensionsを設定する
        reader->SetDimensions( volume_dimensions->c_str() );
        reader->SetOutputTypeToUnstructured();

        // 出力型・時間軸などのメタ情報を構築する
        reader->UpdateInformation();

        // 出力ポート0のパイプライン情報を取得する、出力ポートは基本0
        vtkInformation* output_information = reader->GetOutputInformation( 0 );

        if ( !output_information )
        {
            throw std::runtime_error( "Failed to get vtkNetCDFCFReader output information." );
        }

        // time stepは0を指定しておく
        int time_step = 0;

        // time stepを選択する
        auto* time_steps_key = vtkStreamingDemandDrivenPipeline::TIME_STEPS();

        if ( output_information->Has( time_steps_key ) )
        {
            const int number_of_time_steps = output_information->Length( time_steps_key );

            if ( time_step < 0 || time_step >= number_of_time_steps )
            {
                std::ostringstream message;
                message << "Invalid time step: " << time_step << ". Valid range is [0, " << number_of_time_steps - 1 << "].";
                throw std::out_of_range( message.str() );
            }

            const double requested_time = output_information->Get( time_steps_key, time_step );

            std::cout << "Number of time steps : " << number_of_time_steps << std::endl;
            std::cout << "Selected time step   : " << time_step            << std::endl;
            std::cout << "VTK time             : " << requested_time       << std::endl;

            reader->UpdateTimeStep( requested_time );
        }
        else
        {
            // time dimensionなしでtime stepを0以外を指定した場合エラー
            if ( time_step != 0 )
            {
                throw std::out_of_range( "This NetCDF file has no time dimension. Only time_step = 0 is valid." );
            }

            std::cout << "No time dimension found." << std::endl;

            reader->Update();
        }

        vtkSmartPointer<vtkUnstructuredGrid> grid = vtkUnstructuredGrid::SafeDownCast( reader->GetOutputDataObject( 0 ) );

        if ( !grid )
        {
            throw std::runtime_error( "vtkNetCDFCFReader did not produce vtkUnstructuredGrid." );
        }

        return grid;
    }

    vtkSmartPointer<vtkUnstructuredGrid> readNetCDFCAM(
        const std::string& points_file_path,
        const std::string& connectivity_file_path )
    {
        if ( points_file_path.empty() )
        {
            throw std::invalid_argument( "CAM data file path is empty." );
        }

        if ( connectivity_file_path.empty() )
        {
            throw std::invalid_argument( "CAM connectivity file path is empty." );
        }

        vtkNew<vtkNetCDFCAMReader> reader;
        reader->SetFileName( points_file_path.c_str() );
        reader->SetConnectivityFileName( connectivity_file_path.c_str() );

        // 全midpoint layerを読み込み、3次元の六面体格子を生成する。
        reader->SetVerticalDimension(
            vtkNetCDFCAMReader::VERTICAL_DIMENSION_MIDPOINT_LAYERS );
        reader->SingleMidpointLayerOff();

        // 出力型・時間軸などのメタ情報を構築する。
        reader->UpdateInformation();

        vtkInformation* output_information = reader->GetOutputInformation( 0 );

        if ( !output_information )
        {
            throw std::runtime_error( "Failed to get vtkNetCDFCAMReader output information." );
        }

        // 1ファイルを1 time stepとして扱い、ファイル内の先頭time stepのみを読み込む。
        auto* time_steps_key = vtkStreamingDemandDrivenPipeline::TIME_STEPS();

        if ( output_information->Has( time_steps_key ) )
        {
            const int number_of_time_steps = output_information->Length( time_steps_key );

            if ( number_of_time_steps < 1 )
            {
                throw std::runtime_error( "No time steps found in CAM data file: " + points_file_path );
            }

            const double requested_time = output_information->Get( time_steps_key, 0 );
            reader->UpdateTimeStep( requested_time );
        }
        else
        {
            reader->Update();
        }

        vtkSmartPointer<vtkUnstructuredGrid> grid =
            vtkUnstructuredGrid::SafeDownCast( reader->GetOutputDataObject( 0 ) );

        if ( !grid )
        {
            throw std::runtime_error(
                "vtkNetCDFCAMReader did not produce vtkUnstructuredGrid: " + points_file_path );
        }

        return grid;
    }

    /**
     * MPAS形式のNetCDFファイルを専用Readerへ委譲して読み込む。
     *
     * \param[in] filename MPASデータファイルのパス。
     * \return 生成したVTK非構造格子。
     * \throws std::exception ファイルの読み込み、形式検証、格子生成、または
     *         セルデータから点データへの変換に失敗した場合。
     */
    vtkSmartPointer<vtkUnstructuredGrid> readNetCDFMPAS( const std::string& filename )
    {
        DirectNetCDFMPASReader reader( filename, m_layer_thickness, m_is_atmosphere );
        return reader.read();
    }

private:
    ReaderType m_reader_type;
    std::string m_sub_file_path; // connectivity file (CAM) or mode file (SLAC)
    int m_layer_thickness; // for MPAS
    bool m_is_atmosphere; // for MPAS
    vtkSmartPointer<vtkUnstructuredGrid> vtk_data;
};
} // namespace cvt

#endif // CVT__NETCDF_H_INCLUDE
