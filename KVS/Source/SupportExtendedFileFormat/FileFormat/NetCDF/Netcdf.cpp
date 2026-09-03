/*
 * Copyright (c) 2026 Japan Atomic Energy Agency
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Netcdf.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <kvs/Message>
#include <kvs/Type>
#include <kvs/extendedfileformat/RectilinearGridToUnstructured>
#include <kvs/extendedfileformat/VtkXmlImageData>
#include <kvs/extendedfileformat/VtkXmlPolyData>
#include <kvs/extendedfileformat/VtkXmlRectilinearGrid>
#include <kvs/extendedfileformat/VtkXmlStructuredGrid>
#include <kvs/extendedfileformat/VtkXmlUnstructuredGrid>
#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCellDataToPointData.h>
#include <vtkCellType.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkErrorCode.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkInformation.h>
#include <vtkNetCDFCFReader.h>
#include <vtkNetCDFPOPReader.h>
#include <vtkNetCDFReader.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkRectilinearGrid.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkStructuredGrid.h>
#include <vtkStringArray.h>
#include <vtkUnstructuredGrid.h>
#include <vtk_netcdf.h>

namespace kvs
{
namespace ExtendedFileFormat
{
namespace detail
{

struct NetcdfDiagnostics
{
    std::string phase;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct NetcdfDimensions
{
    vtkIdType x = 0;
    vtkIdType y = 0;
    vtkIdType z = 0;
};

/**
 * @brief VTKリーダーが通知したエラーまたは警告を診断情報へ記録する。
 * @param event_id VTKイベントID。
 * @param client_data 診断情報へのポインター。
 * @param call_data VTKが通知したメッセージ。
 */
void OnNetcdfVtkMessage( vtkObject*, unsigned long event_id, void* client_data, void* call_data )
{
    auto* diagnostics = static_cast<NetcdfDiagnostics*>( client_data );
    const char* message = static_cast<const char*>( call_data );
    std::ostringstream text;
    text << diagnostics->phase << ": " << ( message ? message : "(no message)" );

    if ( event_id == vtkCommand::ErrorEvent )
    {
        diagnostics->errors.push_back( text.str() );
    }
    else
    {
        diagnostics->warnings.push_back( text.str() );
    }
}

/**
 * @brief NetCDFリーダーにエラー・警告監視用コールバックを登録する。
 * @param reader 監視対象のNetCDFリーダー。
 * @param callback 登録するVTKコールバック。
 * @param diagnostics 診断情報の格納先。
 */
void ObserveNetcdfReader( vtkNetCDFCFReader* reader, vtkCallbackCommand* callback,
                          NetcdfDiagnostics& diagnostics )
{
    callback->SetClientData( &diagnostics );
    callback->SetCallback( OnNetcdfVtkMessage );
    reader->AddObserver( vtkCommand::ErrorEvent, callback );
    reader->AddObserver( vtkCommand::WarningEvent, callback );
}

/**
 * @brief 必須変数の存在と次元を検証し、読み込み対象として選択する。
 * @param reader 設定対象のNetCDFリーダー。
 * @param dimensions 必須の次元文字列。
 * @param variable_names 読み込む変数名の一覧。
 * @param phase 診断メッセージに付加する処理名。
 * @param diagnostics 診断情報の格納先。
 * @return すべての必須変数が条件を満たす場合はtrue、それ以外はfalse。
 */
bool SelectNetcdfVariables( vtkNetCDFCFReader* reader, const std::string& dimensions,
                            const std::vector<std::string>& variable_names,
                            const std::string& phase, NetcdfDiagnostics& diagnostics )
{
    // 以降に発生するVTKメッセージがメタデータ取得中のものだと分かるよう処理名を設定する。
    diagnostics.phase = phase + " metadata";

    // 変数名や次元を検証する前に、ファイルからメタデータだけを読み込む。
    if ( reader->UpdateMetaData() == 0 )
    {
        diagnostics.errors.push_back( diagnostics.phase + ": failed to read NetCDF metadata" );
        return false;
    }

    // VTKが保持する各変数の次元文字列を、変数一覧と同じ添字で参照する。
    vtkStringArray* variable_dimensions = reader->GetVariableDimensions();
    bool valid = true;

    // 呼び出し側が指定した必須変数を一つずつNetCDFファイル内から検索する。
    for ( const auto& variable_name : variable_names )
    {
        bool found = false;
        for ( int i = 0; i < reader->GetNumberOfVariableArrays(); ++i )
        {
            if ( variable_name == reader->GetVariableArrayName( i ) )
            {
                found = true;

                // 同名の変数でも次元が異なる場合は対象形式の変数として扱わない。
                const std::string actual_dimensions = variable_dimensions->GetValue( i );
                if ( actual_dimensions != dimensions )
                {
                    diagnostics.errors.push_back(
                        phase + ": variable " + variable_name + " has dimensions " +
                        actual_dimensions + "; expected " + dimensions );
                    valid = false;
                }
                break;
            }
        }
        if ( !found )
        {
            // 必須変数が存在しないことを記録し、ほかの必須変数の検証を続ける。
            diagnostics.errors.push_back( phase + ": required variable " + variable_name +
                                          " was not found" );
            valid = false;
        }
    }
    if ( !valid )
    {
        // 一つでも条件を満たさない変数があれば、読み込み対象の設定は変更しない。
        return false;
    }

    // 検証済みの必須変数だけをリーダーの出力対象にする。
    // まず全変数を無効化し、不要な配列が出力へ含まれないようにする。
    for ( int i = 0; i < reader->GetNumberOfVariableArrays(); ++i )
    {
        reader->SetVariableArrayStatus( reader->GetVariableArrayName( i ), 0 );
    }

    // 必須変数だけを再度有効化する。
    for ( const auto& variable_name : variable_names )
    {
        reader->SetVariableArrayStatus( variable_name.c_str(), 1 );
    }
    return true;
}

/**
 * @brief データセットから必須の点データ配列を取得する。
 * @param data_set 検索対象のデータセット。
 * @param name 配列名。
 * @param phase 診断メッセージに付加する処理名。
 * @param diagnostics 診断情報の格納先。
 * @return 配列が存在する場合はそのポインター、存在しない場合はnullptr。
 */
vtkDataArray* GetRequiredNetcdfArray( vtkDataSet* data_set, const char* name,
                                      const std::string& phase,
                                      NetcdfDiagnostics& diagnostics )
{
    if ( !data_set )
    {
        diagnostics.errors.push_back( phase + ": the reader returned no data set" );
        return nullptr;
    }

    auto* array = data_set->GetPointData()->GetArray( name );
    if ( !array )
    {
        diagnostics.errors.push_back( phase + ": point-data array " + name +
                                      " was not loaded" );
    }
    return array;
}

/**
 * @brief VTKリーダーのエラーコードを確認して診断情報へ記録する。
 * @param reader 確認対象のNetCDFリーダー。
 * @param phase 診断メッセージに付加する処理名。
 * @param diagnostics 診断情報の格納先。
 * @return エラーがない場合はtrue、エラーがある場合はfalse。
 */
bool CheckNetcdfReaderError( vtkNetCDFCFReader* reader, const std::string& phase,
                             NetcdfDiagnostics& diagnostics )
{
    const unsigned long error_code = reader->GetErrorCode();
    if ( error_code == vtkErrorCode::NoError )
    {
        return true;
    }

    std::ostringstream message;
    message << phase << ": VTK error code " << error_code << " ("
            << vtkErrorCode::GetStringFromErrorCode( error_code ) << ")";
    diagnostics.errors.push_back( message.str() );
    return false;
}

/**
 * @brief 警告を出力し、エラーが記録されていれば例外を送出する。
 * @param diagnostics 出力対象の診断情報。
 * @throws std::runtime_error 一つ以上のエラーが記録されている場合。
 */
void ThrowNetcdfDiagnostics( const NetcdfDiagnostics& diagnostics )
{
    for ( const auto& warning : diagnostics.warnings )
    {
        kvsMessageWarning( warning.c_str() );
    }

    if ( !diagnostics.errors.empty() )
    {
        std::ostringstream message;
        message << "Failed to read NetCDF";
        for ( const auto& error : diagnostics.errors )
        {
            message << "\n  " << error;
        }
        throw std::runtime_error( message.str() );
    }
}

/** 文字列をASCII小文字へ変換する。 */
std::string Lowercase( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(),
                    []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
    return value;
}

/** 次元表記に含まれる次元数を返す。 */
std::size_t DimensionRank( const std::string& dimensions )
{
    if ( dimensions.size() < 2 ) return 0;
    return 1 + static_cast<std::size_t>(
                   std::count( dimensions.begin(), dimensions.end(), ',' ) );
}

/** VTKデータセットのセルデータを点データへ変換する。 */
vtkSmartPointer<vtkDataSet> PointCenteredDataSet( vtkDataSet* input )
{
    if ( !input ) return nullptr;
    vtkNew<vtkCellDataToPointData> converter;
    converter->SetInputData( input );
    converter->PassCellDataOff();
    converter->Update();
    vtkSmartPointer<vtkDataSet> output = vtkDataSet::SafeDownCast( converter->GetOutput() );
    return output;
}

/** VTKデータセットの実際の型に対応するKVSファイル形式ラッパーを生成する。 */
std::shared_ptr<kvs::FileFormatBase> WrapDataSet( vtkDataSet* input )
{
    auto data = PointCenteredDataSet( input );
    if ( !data || data->GetNumberOfPoints() == 0 || data->GetNumberOfCells() == 0 )
    {
        throw std::runtime_error( "the VTK NetCDF reader returned an empty data set" );
    }
    if ( auto* image = vtkImageData::SafeDownCast( data ) )
    {
        return std::make_shared<VtkXmlImageData>( image );
    }
    if ( auto* rectilinear = vtkRectilinearGrid::SafeDownCast( data ) )
    {
        return std::make_shared<VtkXmlRectilinearGrid>( rectilinear );
    }
    if ( auto* structured = vtkStructuredGrid::SafeDownCast( data ) )
    {
        return std::make_shared<VtkXmlStructuredGrid>( structured );
    }
    if ( auto* unstructured = vtkUnstructuredGrid::SafeDownCast( data ) )
    {
        return std::make_shared<VtkXmlUnstructuredGrid>( unstructured );
    }
    if ( auto* poly = vtkPolyData::SafeDownCast( data ) )
    {
        return std::make_shared<VtkXmlPolyData>( poly );
    }
    throw std::runtime_error( std::string( "unsupported VTK NetCDF output type: " ) +
                              data->GetClassName() );
}

/** VTKが公開する先頭の物理時刻を更新時刻として指定する。 */
void SelectFirstTimeStep( vtkAlgorithm* reader )
{
    reader->UpdateInformation();
    vtkInformation* output_information = reader->GetOutputInformation( 0 );
    if ( output_information &&
         output_information->Has( vtkStreamingDemandDrivenPipeline::TIME_STEPS() ) &&
         output_information->Length( vtkStreamingDemandDrivenPipeline::TIME_STEPS() ) > 0 )
    {
        const double first_time =
            output_information->Get( vtkStreamingDemandDrivenPipeline::TIME_STEPS(), 0 );
        output_information->Set( vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP(),
                                 first_time );
    }
}

class GearnNetcdfFormatAdapter : public NetcdfFormatAdapter
{
public:
    /// 対応形式名を返す。
    const char* name() const override { return "GEARN"; }
    /// GEARN形式種別を返す。
    NetcdfFormatType formatType() const override { return NetcdfFormatType::Gearn; }
    /// GEARNデータの変換先格子種別を返す。
    NetcdfGridType gridType() const override { return NetcdfGridType::UnstructuredGrid; }

    /**
     * @brief メタデータがGEARN形式に必要な変数構成を持つかを判定する。
     * @param metadata 判定対象の変数メタデータ。
     * @return GEARN形式の条件を満たす場合はtrue、それ以外はfalse。
     */
    bool matches( const NetcdfMetadata& metadata ) const override
    {
        return metadata.hasVariable( "XDIS", "(NCX)" ) &&
               metadata.hasVariable( "YDIS", "(NCY)" ) &&
               metadata.hasVariable( "ZZT", "(NCZ, NCY, NCX)" ) &&
               metadata.hasVariable( "U", "(NCZ, NCY, NCX)" ) &&
               metadata.hasVariable( "V", "(NCZ, NCY, NCX)" ) &&
               metadata.hasVariable( "W", "(NCZ, NCY, NCX)" ) &&
               metadata.hasVariable( "CONC_Cs137", "(NCZ, NCY, NCX)" );
    }

    /**
     * @brief GEARN形式のNetCDFファイルを非構造格子へ変換して読み込む。
     * @param filename 入力ファイル名。
     * @return 変換後の非構造格子ファイル形式オブジェクト。
     */
    std::shared_ptr<kvs::FileFormatBase> read( const std::string& filename ) const override
    {
        NetcdfDiagnostics diagnostics;
        vtkNew<vtkNetCDFCFReader> x_reader;
        vtkNew<vtkNetCDFCFReader> y_reader;
        vtkNew<vtkNetCDFCFReader> data_reader;
        vtkNew<vtkCallbackCommand> callback;

        // 各リーダーのVTKメッセージを共通の診断情報へ集約する。
        ObserveNetcdfReader( x_reader, callback, diagnostics );
        ObserveNetcdfReader( y_reader, callback, diagnostics );
        ObserveNetcdfReader( data_reader, callback, diagnostics );

        x_reader->SetFileName( filename.c_str() );
        y_reader->SetFileName( filename.c_str() );
        data_reader->SetFileName( filename.c_str() );
        data_reader->SetOutputTypeToUnstructured();

        // 座標軸と3次元物理量を、それぞれ適切な次元を持つ変数に限定する。
        if ( !SelectNetcdfVariables( x_reader, "(NCX)", { "XDIS" }, "XDIS reader",
                                     diagnostics ) ||
             !SelectNetcdfVariables( y_reader, "(NCY)", { "YDIS" }, "YDIS reader",
                                     diagnostics ) ||
             !SelectNetcdfVariables(
                 data_reader, "(NCZ, NCY, NCX)", { "ZZT", "U", "V", "W", "CONC_Cs137" },
                 "3D data reader", diagnostics ) )
        {
            ThrowNetcdfDiagnostics( diagnostics );
        }

        // メタデータ検証後に実データを読み込む。
        diagnostics.phase = "XDIS reader Update";
        x_reader->Update();
        diagnostics.phase = "YDIS reader Update";
        y_reader->Update();
        diagnostics.phase = "3D data reader Update";
        data_reader->Update();

        CheckNetcdfReaderError( x_reader, "XDIS reader", diagnostics );
        CheckNetcdfReaderError( y_reader, "YDIS reader", diagnostics );
        CheckNetcdfReaderError( data_reader, "3D data reader", diagnostics );

        auto* x_data_set = vtkDataSet::SafeDownCast( x_reader->GetOutputDataObject( 0 ) );
        auto* y_data_set = vtkDataSet::SafeDownCast( y_reader->GetOutputDataObject( 0 ) );
        auto* data_grid =
            vtkUnstructuredGrid::SafeDownCast( data_reader->GetOutputDataObject( 0 ) );
        auto* xdis = GetRequiredNetcdfArray( x_data_set, "XDIS", "XDIS reader", diagnostics );
        auto* ydis = GetRequiredNetcdfArray( y_data_set, "YDIS", "YDIS reader", diagnostics );
        auto* zzt = GetRequiredNetcdfArray( data_grid, "ZZT", "3D data reader", diagnostics );
        auto* u = GetRequiredNetcdfArray( data_grid, "U", "3D data reader", diagnostics );
        auto* v = GetRequiredNetcdfArray( data_grid, "V", "3D data reader", diagnostics );
        auto* w = GetRequiredNetcdfArray( data_grid, "W", "3D data reader", diagnostics );
        auto* cs137 =
            GetRequiredNetcdfArray( data_grid, "CONC_Cs137", "3D data reader", diagnostics );
        ThrowNetcdfDiagnostics( diagnostics );

        // 座標配列と3次元格子の要素数が整合していることを確認する。
        const vtkIdType x_size = xdis->GetNumberOfTuples();
        const vtkIdType y_size = ydis->GetNumberOfTuples();
        const vtkIdType number_of_points = data_grid->GetNumberOfPoints();

        // X方向とY方向にセルを構成できる2点以上があり、総格子点数が正であることを確認する。
        // また、XY平面の点数をオーバーフローせずに計算でき、総格子点数が
        // XY平面1層分の点数で割り切れることを確認する。
        if ( x_size < 2 || y_size < 2 || number_of_points <= 0 ||
             x_size > std::numeric_limits<vtkIdType>::max() / y_size ||
             number_of_points % ( x_size * y_size ) != 0 )
        {
            throw std::runtime_error(
                "GEARN NetCDF coordinate assembly: NCX, NCY, and 3D point counts are "
                "inconsistent" );
        }

        NetcdfDimensions dimensions;
        dimensions.x = x_size;
        dimensions.y = y_size;
        dimensions.z = number_of_points / ( x_size * y_size );
        if ( dimensions.z < 2 )
        {
            throw std::runtime_error( "GEARN NetCDF coordinate assembly: NCZ must be at least 2" );
        }

        const vtkIdType number_of_cells =
            ( dimensions.x - 1 ) * ( dimensions.y - 1 ) * ( dimensions.z - 1 );
        if ( data_grid->GetNumberOfCells() != number_of_cells ||
             zzt->GetNumberOfTuples() != number_of_points ||
             u->GetNumberOfTuples() != number_of_points ||
             v->GetNumberOfTuples() != number_of_points ||
             w->GetNumberOfTuples() != number_of_points ||
             cs137->GetNumberOfTuples() != number_of_points )
        {
            throw std::runtime_error(
                "GEARN NetCDF coordinate assembly: point-data or hexahedral-cell counts are "
                "inconsistent" );
        }

        // KVSへ渡す物理量はすべて1成分の点データでなければならない。
        vtkDataArray* value_arrays[] = { u, v, w, cs137 };
        const char* value_names[] = { "U", "V", "W", "CONC_Cs137" };
        for ( std::size_t i = 0; i < 4; ++i )
        {
            if ( value_arrays[i]->GetNumberOfComponents() != 1 )
            {
                throw std::runtime_error( std::string( "GEARN NetCDF point-data array " ) +
                                          value_names[i] + " must have one component" );
            }
        }
        for ( vtkIdType i = 0; i < number_of_cells; ++i )
        {
            if ( data_grid->GetCellType( i ) != VTK_HEXAHEDRON )
            {
                throw std::runtime_error(
                    "GEARN NetCDF grid contains a non-hexahedral VTK cell" );
            }
        }
        if ( static_cast<unsigned long long>( number_of_points - 1 ) >
             std::numeric_limits<kvs::UInt32>::max() )
        {
            throw std::runtime_error(
                "GEARN NetCDF node count exceeds the KVS UInt32 connection limit" );
        }

        // 1次元のX/Y座標と3次元のZ座標から各格子点の座標を再構成する。
        vtkNew<vtkFloatArray> point_array;
        point_array->SetNumberOfComponents( 3 );
        point_array->SetNumberOfTuples( number_of_points );
        float* point_values = point_array->GetPointer( 0 );

#pragma omp parallel for
        for ( vtkIdType k = 0; k < dimensions.z; ++k )
        {
            for ( vtkIdType j = 0; j < dimensions.y; ++j )
            {
                for ( vtkIdType i = 0; i < dimensions.x; ++i )
                {
                    const vtkIdType id =
                        i + j * dimensions.x + k * dimensions.x * dimensions.y;
                    point_values[id * 3] = static_cast<float>( xdis->GetComponent( i, 0 ) );
                    point_values[id * 3 + 1] =
                        static_cast<float>( ydis->GetComponent( j, 0 ) );
                    point_values[id * 3 + 2] =
                        static_cast<float>( zzt->GetComponent( id, 0 ) );
                }
            }
        }

        // 再構成した座標と必要な物理量だけを持つ非構造格子を生成する。
        vtkNew<vtkPoints> points;
        points->SetData( point_array );
        vtkSmartPointer<vtkUnstructuredGrid> normalized =
            vtkSmartPointer<vtkUnstructuredGrid>::New();
        normalized->ShallowCopy( data_grid );
        normalized->SetPoints( points );
        normalized->GetPointData()->Initialize();
        normalized->GetPointData()->AddArray( u );
        normalized->GetPointData()->AddArray( v );
        normalized->GetPointData()->AddArray( w );
        normalized->GetPointData()->AddArray( cs137 );

        return std::make_shared<VtkXmlUnstructuredGrid>( normalized.GetPointer() );
    }

};

/** CF/COARDSファイルで最も高次元の物理量群を選択する。 */
bool SelectHighestRankDimensions( vtkNetCDFReader* reader )
{
    if ( reader->UpdateMetaData() == 0 ) return false;
    vtkStringArray* dimensions = reader->GetVariableDimensions();
    std::string selected;
    std::size_t selected_rank = 0;
    for ( int i = 0; i < reader->GetNumberOfVariableArrays(); ++i )
    {
        const std::string current = dimensions->GetValue( i );
        const std::size_t rank = DimensionRank( current );
        if ( rank > selected_rank )
        {
            selected = current;
            selected_rank = rank;
        }
    }
    if ( selected.empty() ) return false;
    reader->SetDimensions( selected.c_str() );
    return true;
}

/** vtkNetCDFReaderの次元表記を外側から順に分解する。 */
std::vector<std::string> ParseDimensionNames( const std::string& dimensions )
{
    const auto begin = dimensions.find( '(' );
    const auto end = dimensions.rfind( ')' );
    const std::size_t content_begin = begin == std::string::npos ? 0 : begin + 1;
    const std::size_t content_end = end == std::string::npos ? dimensions.size() : end;
    if ( content_begin >= content_end ) return {};

    std::vector<std::string> names;
    std::stringstream stream( dimensions.substr( content_begin, content_end - content_begin ) );
    std::string name;
    while ( std::getline( stream, name, ',' ) )
    {
        const auto first = name.find_first_not_of( " \t\r\n" );
        const auto last = name.find_last_not_of( " \t\r\n" );
        if ( first == std::string::npos ) return {};
        names.push_back( name.substr( first, last - first + 1 ) );
    }
    return names;
}

/** NetCDFメタデータから最大ランク物理量の次元名を返す。 */
std::vector<std::string> HighestRankDimensionNames( const NetcdfMetadata& metadata )
{
    std::string selected;
    std::size_t selected_rank = 0;
    for ( const auto& variable : metadata.variableDimensions() )
    {
        const std::size_t rank = DimensionRank( variable.second );
        if ( rank > selected_rank )
        {
            selected = variable.second;
            selected_rank = rank;
        }
    }
    return ParseDimensionNames( selected );
}

/** 同名1次元NetCDF座標変数を、単位変換せずdouble値として読む。 */
std::vector<double> ReadCoordinateVariable( int file, const std::string& dimension_name,
                                            std::size_t expected_length )
{
    int dimension = -1;
    std::size_t dimension_length = 0;
    int variable = -1;
    nc_type variable_type = NC_NAT;
    int rank = 0;
    int variable_dimensions[NC_MAX_VAR_DIMS] = {};
    if ( nc_inq_dimid( file, dimension_name.c_str(), &dimension ) != NC_NOERR ||
         nc_inq_dimlen( file, dimension, &dimension_length ) != NC_NOERR ||
         dimension_length != expected_length )
    {
        throw std::runtime_error( "coordinate count does not match dimension " +
                                  dimension_name );
    }
    if ( nc_inq_varid( file, dimension_name.c_str(), &variable ) != NC_NOERR ||
         nc_inq_var( file, variable, nullptr, &variable_type, &rank, variable_dimensions,
                     nullptr ) != NC_NOERR ||
         rank != 1 || variable_dimensions[0] != dimension )
    {
        throw std::runtime_error( "dimension " + dimension_name +
                                  " requires a same-named 1D coordinate variable" );
    }
    if ( variable_type == NC_CHAR
#ifdef NC_STRING
         || variable_type == NC_STRING
#endif
    )
    {
        throw std::runtime_error( "coordinate variable " + dimension_name +
                                  " is not numeric" );
    }

    std::vector<double> coordinates( expected_length );
    const int error = nc_get_var_double( file, variable, coordinates.data() );
    if ( error != NC_NOERR )
    {
        throw std::runtime_error( "failed to read coordinate variable " + dimension_name +
                                  ": " + nc_strerror( error ) );
    }
    return coordinates;
}

/** Generic NetCDFの3本の物理座標軸をx、y、z順で読む。 */
std::vector<std::vector<double>> ReadGenericRectilinearCoordinates(
    const std::string& filename, const std::vector<std::string>& dimension_names,
    const int image_dimensions[3] )
{
    if ( dimension_names.size() != 4 || Lowercase( dimension_names[0] ) != "time" ||
         Lowercase( dimension_names[1] ) != "z" ||
         Lowercase( dimension_names[2] ) != "y" ||
         Lowercase( dimension_names[3] ) != "x" )
    {
        throw std::runtime_error(
            "generic rectilinear NetCDF requires dimensions (time, z, y, x)" );
    }

    int file = -1;
    const int open_error = nc_open( filename.c_str(), NC_NOWRITE, &file );
    if ( open_error != NC_NOERR )
    {
        throw std::runtime_error( std::string( "failed to open coordinates: " ) +
                                  nc_strerror( open_error ) );
    }
    try
    {
        std::vector<std::vector<double>> coordinates( 3 );
        coordinates[0] = ReadCoordinateVariable(
            file, dimension_names[3], static_cast<std::size_t>( image_dimensions[0] ) );
        coordinates[1] = ReadCoordinateVariable(
            file, dimension_names[2], static_cast<std::size_t>( image_dimensions[1] ) );
        coordinates[2] = ReadCoordinateVariable(
            file, dimension_names[1], static_cast<std::size_t>( image_dimensions[2] ) );
        nc_close( file );
        return coordinates;
    }
    catch ( ... )
    {
        nc_close( file );
        throw;
    }
}

class CfNetcdfFormatAdapter : public NetcdfFormatAdapter
{
public:
    const char* name() const override { return "VTK CF"; }
    NetcdfFormatType formatType() const override { return NetcdfFormatType::Cf; }
    NetcdfGridType gridType() const override { return NetcdfGridType::UnstructuredGrid; }
    bool matches( const NetcdfMetadata& metadata ) const override
    {
        return Lowercase( metadata.globalAttribute( "Conventions" ) ).find( "cf-" ) !=
               std::string::npos;
    }
    std::shared_ptr<kvs::FileFormatBase> read( const std::string& filename ) const override
    {
        vtkNew<vtkNetCDFCFReader> reader;
        reader->SetFileName( filename.c_str() );
        if ( !SelectHighestRankDimensions( reader ) )
        {
            throw std::runtime_error( "vtkNetCDFCFReader found no readable data variable" );
        }
        reader->SetOutputTypeToUnstructured();
        SelectFirstTimeStep( reader );
        reader->Update();
        return WrapDataSet( vtkDataSet::SafeDownCast( reader->GetOutputDataObject( 0 ) ) );
    }
};

class PopNetcdfFormatAdapter : public NetcdfFormatAdapter
{
public:
    const char* name() const override { return "VTK POP"; }
    NetcdfFormatType formatType() const override { return NetcdfFormatType::Pop; }
    NetcdfGridType gridType() const override { return NetcdfGridType::UnstructuredGrid; }
    bool matches( const NetcdfMetadata& metadata ) const override
    {
        if ( metadata.hasDimension( "time" ) || metadata.hasDimension( "Time" ) )
            return false;
        for ( const auto& variable : metadata.variableDimensions() )
        {
            if ( DimensionRank( variable.second ) == 3 ) return true;
        }
        return false;
    }
    std::shared_ptr<kvs::FileFormatBase> read( const std::string& filename ) const override
    {
        vtkNew<vtkNetCDFPOPReader> reader;
        reader->SetFileName( filename.c_str() );
        reader->Update();
        auto point_centered = PointCenteredDataSet(
            vtkDataSet::SafeDownCast( reader->GetOutputDataObject( 0 ) ) );
        auto* rectilinear = vtkRectilinearGrid::SafeDownCast( point_centered );
        if ( !rectilinear )
        {
            throw std::runtime_error(
                "vtkNetCDFPOPReader did not return a vtkRectilinearGrid" );
        }
        auto unstructured = RectilinearGridToLinearHexahedra( rectilinear );
        return std::make_shared<VtkXmlUnstructuredGrid>( unstructured );
    }
};

class GenericNetcdfFormatAdapter : public NetcdfFormatAdapter
{
public:
    const char* name() const override { return "VTK generic"; }
    NetcdfFormatType formatType() const override { return NetcdfFormatType::Generic; }
    NetcdfGridType gridType() const override { return NetcdfGridType::UnstructuredGrid; }
    bool matches( const NetcdfMetadata& metadata ) const override
    {
        return !metadata.variableDimensions().empty();
    }
    std::shared_ptr<kvs::FileFormatBase> read( const std::string& filename ) const override
    {
        vtkNew<vtkNetCDFReader> reader;
        reader->SetFileName( filename.c_str() );
        if ( !SelectHighestRankDimensions( reader ) )
        {
            throw std::runtime_error( "vtkNetCDFReader found no readable data variable" );
        }
        NetcdfMetadata metadata;
        if ( !Netcdf::ReadMetadata( filename, metadata ) )
        {
            throw std::runtime_error( "failed to read Generic NetCDF dimensions" );
        }
        const auto dimension_names = HighestRankDimensionNames( metadata );

        SelectFirstTimeStep( reader );
        reader->Update();
        auto* image = vtkImageData::SafeDownCast( reader->GetOutputDataObject( 0 ) );
        if ( !image )
        {
            throw std::runtime_error( "vtkNetCDFReader did not return vtkImageData" );
        }

        int image_dimensions[3] = {};
        image->GetDimensions( image_dimensions );
        auto coordinates =
            ReadGenericRectilinearCoordinates( filename, dimension_names, image_dimensions );

        vtkNew<vtkImageData> point_fields;
        point_fields->ShallowCopy( image );
        for ( const auto& name : dimension_names )
        {
            point_fields->GetPointData()->RemoveArray( name.c_str() );
        }
        auto unstructured = RectilinearGridToLinearHexahedra(
            coordinates[0], coordinates[1], coordinates[2], point_fields );
        return std::make_shared<VtkXmlUnstructuredGrid>( unstructured );
    }
};

/**
 * @brief 利用可能なNetCDF形式アダプターの一覧を返す。
 * @return 登録済みアダプターの一覧。
 */
const std::vector<std::shared_ptr<NetcdfFormatAdapter>>& RegisteredNetcdfAdapters()
{
    static const std::vector<std::shared_ptr<NetcdfFormatAdapter>> adapters = {
        std::make_shared<GearnNetcdfFormatAdapter>(),
        std::make_shared<CfNetcdfFormatAdapter>(),
        std::make_shared<PopNetcdfFormatAdapter>(),
        std::make_shared<GenericNetcdfFormatAdapter>()
    };
    return adapters;
}

/**
 * @brief 変換結果の実際の型がアダプターの格子種別と一致するかを判定する。
 * @param format 変換後のファイル形式オブジェクト。
 * @param grid_type 期待する格子種別。
 * @return 型が一致する場合はtrue、それ以外はfalse。
 */
bool MatchesNetcdfGridType( const std::shared_ptr<kvs::FileFormatBase>& format,
                            NetcdfGridType grid_type )
{
    switch ( grid_type )
    {
    case NetcdfGridType::ImageData:
        return dynamic_cast<VtkXmlImageData*>( format.get() ) != nullptr;
    case NetcdfGridType::RectilinearGrid:
        return dynamic_cast<VtkXmlRectilinearGrid*>( format.get() ) != nullptr;
    case NetcdfGridType::StructuredGrid:
        return dynamic_cast<VtkXmlStructuredGrid*>( format.get() ) != nullptr;
    case NetcdfGridType::UnstructuredGrid:
        return dynamic_cast<VtkXmlUnstructuredGrid*>( format.get() ) != nullptr;
    case NetcdfGridType::PolyData:
        return dynamic_cast<VtkXmlPolyData*>( format.get() ) != nullptr;
    case NetcdfGridType::Unknown:
    default:
        return false;
    }
}
} // namespace detail

/**
 * @brief 指定した名前と次元を持つ変数がメタデータに存在するかを判定する。
 */
bool NetcdfMetadata::hasVariable( const std::string& name, const std::string& dimensions ) const
{
    const auto found = m_variable_dimensions.find( name );
    return found != m_variable_dimensions.end() && found->second == dimensions;
}

/**
 * @brief 指定した名前の変数がメタデータに存在するかを判定する。
 */
bool NetcdfMetadata::hasVariable( const std::string& name ) const
{
    return m_variable_dimensions.find( name ) != m_variable_dimensions.end();
}

/**
 * @brief 指定した名前の次元がメタデータに存在するかを判定する。
 */
bool NetcdfMetadata::hasDimension( const std::string& name ) const
{
    return m_dimensions.find( name ) != m_dimensions.end();
}

/**
 * @brief 指定した変数のNetCDF型を返す。
 */
int NetcdfMetadata::variableType( const std::string& name ) const
{
    const auto found = m_variable_types.find( name );
    return found == m_variable_types.end() ? NC_NAT : found->second;
}

/**
 * @brief 指定した変数の各次元長を返す。
 */
const std::vector<std::size_t>& NetcdfMetadata::variableShape( const std::string& name ) const
{
    static const std::vector<std::size_t> empty;
    const auto found = m_variable_shapes.find( name );
    return found == m_variable_shapes.end() ? empty : found->second;
}

/**
 * @brief 指定した変数の文字列属性を返す。
 */
std::string NetcdfMetadata::variableAttribute( const std::string& variable,
                                               const std::string& attribute ) const
{
    const auto found_variable = m_variable_attributes.find( variable );
    if ( found_variable == m_variable_attributes.end() ) return "";
    const auto found_attribute = found_variable->second.find( attribute );
    return found_attribute == found_variable->second.end() ? "" : found_attribute->second;
}

/**
 * @brief 指定したグローバル文字列属性を返す。
 */
std::string NetcdfMetadata::globalAttribute( const std::string& attribute ) const
{
    const auto found = m_global_attributes.find( attribute );
    return found == m_global_attributes.end() ? "" : found->second;
}

/**
 * @brief NetCDFファイルから変数名と次元の対応を読み込む。
 * @param filename 入力ファイル名。
 * @param metadata 読み込んだメタデータの格納先。
 * @return 読み込みに成功した場合はtrue、それ以外はfalse。
 */
bool Netcdf::ReadMetadata( const std::string& filename, NetcdfMetadata& metadata )
{
    int file = -1;
    const int open_error = nc_open( filename.c_str(), NC_NOWRITE, &file );
    if ( open_error != NC_NOERR )
    {
        kvsMessageError( ( std::string( "Failed to open NetCDF metadata: " ) +
                           nc_strerror( open_error ) + ": " + filename )
                             .c_str() );
        return false;
    }

    auto close_file = [&]() {
        if ( file >= 0 )
        {
            nc_close( file );
            file = -1;
        }
    };
    auto fail = [&]( const std::string& phase, int error ) {
        kvsMessageError( ( std::string( "Failed to read NetCDF " ) + phase + ": " +
                           nc_strerror( error ) + ": " + filename )
                             .c_str() );
        close_file();
        return false;
    };
    auto read_text_attribute = [&]( int variable, const char* name ) {
        nc_type type = NC_NAT;
        std::size_t length = 0;
        if ( nc_inq_att( file, variable, name, &type, &length ) != NC_NOERR )
            return std::string();
        if ( type == NC_CHAR )
        {
            std::string value( length, '\0' );
            if ( length > 0 && nc_get_att_text( file, variable, name, value.data() ) != NC_NOERR )
                return std::string();
            return value;
        }
#ifdef NC_STRING
        if ( type == NC_STRING && length > 0 )
        {
            std::vector<char*> values( length, nullptr );
            if ( nc_get_att_string( file, variable, name, values.data() ) != NC_NOERR )
                return std::string();
            const std::string result = values.front() ? values.front() : "";
            nc_free_string( length, values.data() );
            return result;
        }
#endif
        return std::string();
    };

    metadata.m_variable_dimensions.clear();
    metadata.m_variable_types.clear();
    metadata.m_variable_shapes.clear();
    metadata.m_dimensions.clear();
    metadata.m_global_attributes.clear();
    metadata.m_variable_attributes.clear();

    int dimension_count = 0;
    int variable_count = 0;
    int global_attribute_count = 0;
    int unlimited_dimension = -1;
    int error = nc_inq( file, &dimension_count, &variable_count, &global_attribute_count,
                        &unlimited_dimension );
    if ( error != NC_NOERR ) return fail( "header", error );

    for ( int i = 0; i < dimension_count; ++i )
    {
        char name[NC_MAX_NAME + 1] = {};
        std::size_t length = 0;
        error = nc_inq_dim( file, i, name, &length );
        if ( error != NC_NOERR ) return fail( "dimension", error );
        metadata.m_dimensions.emplace( name, length );
    }

    for ( int i = 0; i < global_attribute_count; ++i )
    {
        char name[NC_MAX_NAME + 1] = {};
        error = nc_inq_attname( file, NC_GLOBAL, i, name );
        if ( error != NC_NOERR ) return fail( "global attribute", error );
        const std::string value = read_text_attribute( NC_GLOBAL, name );
        if ( !value.empty() ) metadata.m_global_attributes.emplace( name, value );
    }

    for ( int variable = 0; variable < variable_count; ++variable )
    {
        char name[NC_MAX_NAME + 1] = {};
        nc_type type = NC_NAT;
        int rank = 0;
        int dimension_ids[NC_MAX_VAR_DIMS] = {};
        int attribute_count = 0;
        error = nc_inq_var( file, variable, name, &type, &rank, dimension_ids,
                            &attribute_count );
        if ( error != NC_NOERR ) return fail( "variable", error );

        std::ostringstream dimensions;
        dimensions << "(";
        for ( int j = 0; j < rank; ++j )
        {
            char dimension_name[NC_MAX_NAME + 1] = {};
            error = nc_inq_dimname( file, dimension_ids[j], dimension_name );
            if ( error != NC_NOERR ) return fail( "variable dimension", error );
            if ( j > 0 ) dimensions << ", ";
            dimensions << dimension_name;
        }
        dimensions << ")";
        metadata.m_variable_dimensions.emplace( name, dimensions.str() );
        metadata.m_variable_types.emplace( name, static_cast<int>( type ) );

        auto& shape = metadata.m_variable_shapes[name];
        shape.reserve( static_cast<std::size_t>( rank ) );
        for ( int j = 0; j < rank; ++j )
        {
            std::size_t length = 0;
            error = nc_inq_dimlen( file, dimension_ids[j], &length );
            if ( error != NC_NOERR ) return fail( "variable dimension length", error );
            shape.push_back( length );
        }

        auto& attributes = metadata.m_variable_attributes[name];
        for ( int j = 0; j < attribute_count; ++j )
        {
            char attribute_name[NC_MAX_NAME + 1] = {};
            error = nc_inq_attname( file, variable, j, attribute_name );
            if ( error != NC_NOERR ) return fail( "variable attribute", error );
            const std::string value = read_text_attribute( variable, attribute_name );
            if ( !value.empty() ) attributes.emplace( attribute_name, value );
        }
    }

    close_file();
    return true;
}

/**
 * @brief メタデータに適合するNetCDF形式アダプターを一つ選択する。
 * @param metadata 判定対象の変数メタデータ。
 * @return 優先順位が最も高い適合アダプター。未対応の場合はnullptr。
 */
const NetcdfFormatAdapter* Netcdf::SelectAdapter( const NetcdfMetadata& metadata )
{
    for ( const auto& adapter : detail::RegisteredNetcdfAdapters() )
    {
        if ( adapter->matches( metadata ) )
        {
            // 特殊規約から汎用規約の順に登録しているため、最初の一致を採用する。
            return adapter.get();
        }
    }

    std::ostringstream message;
    message << "Unsupported NetCDF data format; variables:";
    for ( const auto& variable : metadata.variableDimensions() )
    {
        message << " " << variable.first << variable.second;
    }
    kvsMessageError( message.str().c_str() );
    return nullptr;
}

/**
 * @brief 指定したNetCDFファイルを読み込んでオブジェクトを初期化する。
 * @param filename 入力ファイル名。
 */
Netcdf::Netcdf( const std::string& filename ) { this->read( filename ); }

/**
 * @brief 読み込み条件を指定してNetCDFファイルを読み込む。
 * @param filename 入力ファイル名。
 * @param options 読み込み条件。
 */
Netcdf::Netcdf( const std::string& filename, const NetcdfReadOptions& options )
{
    this->read( filename, options );
}

/**
 * @brief NetCDFファイルの形式を判別し、対応する格子データへ変換する。
 * @param filename 入力ファイル名。
 * @return 読み込みと変換に成功した場合はtrue、それ以外はfalse。
 */
bool Netcdf::read( const std::string& filename )
{
    return this->read( filename, NetcdfReadOptions{} );
}

/**
 * @brief NetCDFファイルの形式を判別し、指定された条件で格子データへ変換する。
 * @param filename 入力ファイル名。
 * @param options 読み込み条件。
 * @return 読み込みと変換に成功した場合はtrue、それ以外はfalse。
 */
bool Netcdf::read( const std::string& filename, const NetcdfReadOptions& options )
{
    // 前回の読み込み結果を破棄し、失敗状態から処理を開始する。
    this->setFilename( filename );
    this->setSuccess( false );
    m_format.reset();
    m_format_name.clear();
    m_format_type = NetcdfFormatType::Unknown;
    m_grid_type = NetcdfGridType::Unknown;

    // メタデータに基づいて入力形式を判別する。
    NetcdfMetadata metadata;
    if ( !ReadMetadata( filename, metadata ) )
    {
        return false;
    }

    const auto* adapter = SelectAdapter( metadata );
    if ( !adapter )
    {
        return false;
    }

    // 選択したアダプターで実データを読み込み、戻り値の格子型も検証する。
    try
    {
        m_format = adapter->read( filename, options );
        if ( !m_format || !detail::MatchesNetcdfGridType( m_format, adapter->gridType() ) )
        {
            kvsMessageError( ( std::string( adapter->name() ) +
                               " NetCDF adapter returned an invalid VTK grid type for " +
                               filename )
                                 .c_str() );
            m_format.reset();
            return false;
        }
        m_format_name = adapter->name();
        m_format_type = adapter->formatType();
        m_grid_type = adapter->gridType();
        this->setSuccess( true );
        return true;
    }
    catch ( const std::exception& e )
    {
        kvsMessageError( ( std::string( adapter->name() ) + " NetCDF: " + e.what() ).c_str() );
        return false;
    }
    catch ( ... )
    {
        kvsMessageError(
            ( std::string( "Unknown error while reading " ) + adapter->name() + " NetCDF" )
                .c_str() );
        return false;
    }
}

/**
 * @brief NetCDFファイルへの書き出し要求を未実装エラーとして処理する。
 * @param filename 出力ファイル名。
 * @return 常にfalse。
 */
bool Netcdf::write( const std::string& filename )
{
    this->setFilename( filename );
    this->setSuccess( false );
    kvsMessageError( "Writing NetCDF has not been implemented" );
    return false;
}

/**
 * @brief NetCDFファイルのメタデータから対応形式と格子種別を判別する。
 * @param filename 判別対象のファイル名。
 * @param info 判別結果の格納先。
 * @return 判別に成功した場合はtrue、それ以外はfalse。
 */
bool Netcdf::Probe( const std::string& filename, NetcdfFileInfo& info )
{
    NetcdfMetadata metadata;
    if ( !ReadMetadata( filename, metadata ) )
    {
        return false;
    }

    const auto* adapter = SelectAdapter( metadata );
    if ( !adapter )
    {
        return false;
    }

    info.path = filename;
    info.format_name = adapter->name();
    info.format_type = adapter->formatType();
    info.grid_type = adapter->gridType();
    info.input_role = NetcdfInputRole::Standard;
    return true;
}
} // namespace ExtendedFileFormat
} // namespace kvs
