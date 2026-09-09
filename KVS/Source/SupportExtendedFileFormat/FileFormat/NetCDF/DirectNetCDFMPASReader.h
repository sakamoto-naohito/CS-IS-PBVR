/*
 * Copyright (c) 2022 Japan Atomic Energy Agency
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

#ifndef EXTENDED_FILE_FORMAT__DIRECT_NETCDF_MPAS_READER_H_INCLUDE
#define EXTENDED_FILE_FORMAT__DIRECT_NETCDF_MPAS_READER_H_INCLUDE

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <vtkSmartPointer.h>
#include <vtkPoints.h>
#include <vtkUnstructuredGrid.h>
#include <vtkType.h>
#include <vtk_netcdf.h>

namespace kvs
{
namespace ExtendedFileFormat
{
/**
 * MPAS形式のNetCDFファイルをNetCDF C APIから直接読み込むReader。
 *
 * MPASのセル中心座標、接続情報、鉛直層情報をVTKの非構造格子へ変換し、
 * 点データとセルデータを読み込む。
 */
class DirectNetCDFMPASReader
{
public:
    /**
     * MPAS Readerを構築する。
     *
     * \param[in] filename MPASデータファイルのパス。
     * \param[in] layer_thickness 鉛直層間隔。
     * \param[in] is_atmosphere 大気データとして鉛直座標を生成するかどうか。
     * \throws std::invalid_argument filenameが空の場合。
     * \throws std::out_of_range layer_thicknessが正でない場合。
     */
    DirectNetCDFMPASReader(
        const std::string& filename,
        int layer_thickness,
        bool is_atmosphere );

    /**
     * MPASファイルを読み込み、VTK非構造格子を生成する。
     *
     * Time次元を持つデータ変数は、従来のMPAS読み込み処理と同じく先頭時刻を読む。
     *
     * \return 読み込んだVTK非構造格子。
     * \throws std::exception NetCDFの読み込み、形式検証、VTK格子生成、
     *         またはデータ変換に失敗した場合。
     */
    vtkSmartPointer<vtkUnstructuredGrid> read();

private:
    struct NcFileGuard
    {
        int id = -1;
        std::string filename;

        /** NetCDFファイルを読み込み専用で開く。 */
        void open( const std::string& path );

        /** 開いているNetCDFファイルを閉じる。 */
        void close();
        ~NcFileGuard();

        NcFileGuard() = default;
        NcFileGuard( const NcFileGuard& ) = delete;
        NcFileGuard& operator=( const NcFileGuard& ) = delete;
    };

    struct DimensionInfo
    {
        int id = -1;
        std::string name;
        size_t size = 0;
    };

    struct VariableInfo
    {
        int id = -1;
        std::string name;
        nc_type type = NC_NAT;
        std::vector<int> dimension_ids;
        std::vector<double> missing_values;
    };

private:
    /**
     * 入力引数を検証する。
     *
     * \throws std::invalid_argument ファイルパスが空の場合。
     * \throws std::out_of_range 鉛直層間隔が正でない場合。
     */
    void validateInput() const;

    /**
     * NetCDF C APIの戻り値を検証し、統一したエラーを送出する。
     *
     * \param[in] status NetCDF C APIの戻り値。
     * \param[in] operation 実行した操作の説明。
     * \param[in] subject 操作対象の説明。
     * \throws std::runtime_error statusが成功値でない場合。
     */
    void checkNetCDFError( int status, const std::string& operation, const std::string& subject ) const;

    /**
     * size_tの加算でオーバーフローしないことを確認する。
     *
     * \param[in] left 左辺値。
     * \param[in] right 右辺値。
     * \param[in] what 計算内容の説明。
     * \return オーバーフローしない場合の加算結果。
     * \throws std::overflow_error オーバーフローする場合。
     */
    size_t checkedAdd( size_t left, size_t right, const std::string& what ) const;

    /**
     * size_tの乗算でオーバーフローしないことを確認する。
     *
     * \param[in] left 左辺値。
     * \param[in] right 右辺値。
     * \param[in] what 計算内容の説明。
     * \return オーバーフローしない場合の乗算結果。
     * \throws std::overflow_error オーバーフローする場合。
     */
    size_t checkedMultiply( size_t left, size_t right, const std::string& what ) const;

    /**
     * size_tの値をvtkIdTypeへ安全に変換する。
     *
     * \param[in] value 変換する値。
     * \param[in] what 値の用途。
     * \return vtkIdTypeへ変換した値。
     * \throws std::overflow_error vtkIdTypeで表現できない場合。
     */
    vtkIdType vtkId( size_t value, const std::string& what ) const;

    /**
     * NetCDFファイルの次元数と変数数を読み込む。
     *
     * \throws std::runtime_error ファイルメタデータを取得できない場合。
     */
    void readFileMetadata();

    /**
     * 指定された次元の名前と大きさを読み込む。
     *
     * \param[in] dimension_id 読み込む次元のID。
     * \param[in] subject エラーメッセージ用の対象名。
     * \return 次元情報。
     * \throws std::runtime_error 次元情報を取得できない場合、または次元の大きさが0の場合。
     */
    DimensionInfo readDimension( int dimension_id, const std::string& subject ) const;

    /**
     * 必須次元を名前で検索する。
     *
     * \param[in] dimension_name 必須次元名。
     * \return 次元情報。
     * \throws std::exception 必須次元が存在しない場合、または次元情報が不正な場合。
     */
    DimensionInfo findRequiredDimension( const std::string& dimension_name ) const;

    /**
     * MPASの必須次元とTime次元を読み込み、派生サイズを計算する。
     *
     * \throws std::exception 次元が不足している場合、またはサイズが不正な場合。
     */
    void readDimensions();

    /**
     * 指定された変数のメタデータを読み込む。
     *
     * \param[in] variable_id 読み込む変数のID。
     * \return 変数情報。
     * \throws std::runtime_error 変数情報または次元数が不正な場合。
     */
    VariableInfo readVariable( int variable_id ) const;

    /**
     * 必須変数を名前で検索する。
     *
     * \param[in] variable_name 必須変数名。
     * \return 変数情報。
     * \throws std::exception 必須変数が存在しない場合、または変数情報が不正な場合。
     */
    VariableInfo findRequiredVariable( const std::string& variable_name ) const;

    /**
     * 変数の次元構成が期待どおりか検証する。
     *
     * \param[in] variable 検証する変数。
     * \param[in] expected_names 期待する次元名の並び。
     * \param[in] expected_sizes 期待する次元サイズの並び。
     * \throws std::invalid_argument 次元数、順序、名前、サイズが一致しない場合。
     */
    void validateVariableDimensions(
        const VariableInfo& variable,
        const std::vector<std::string>& expected_names,
        const std::vector<size_t>& expected_sizes ) const;

    /**
     * 変数が期待する次元構成に一致するか確認する。
     *
     * \param[in] variable 確認する変数。
     * \param[in] expected_names 期待する次元名の並び。
     * \param[in] expected_sizes 期待する次元サイズの並び。
     * \return 一致する場合はtrue。
     * \throws std::exception 次元情報を取得できない場合。
     */
    bool hasVariableDimensions(
        const VariableInfo& variable,
        const std::vector<std::string>& expected_names,
        const std::vector<size_t>& expected_sizes ) const;

    /**
     * NetCDF変数型が整数型か判定する。
     *
     * \param[in] type 判定するNetCDF型。
     * \return 整数型の場合はtrue。
     */
    static bool isIntegerType( nc_type type );

    /**
     * NetCDF変数型が数値型か判定する。
     *
     * \param[in] type 判定するNetCDF型。
     * \return 数値型の場合はtrue。
     */
    static bool isNumericType( nc_type type );

    /**
     * データ変数の_FillValueとmissing_value属性を読み込む。
     *
     * 属性はNetCDFの数値型からdoubleへ変換して保持する。属性がない場合は
     * 空のままとし、既存の属性を持たないMPASファイルの動作を維持する。
     *
     * \param[in,out] variable 属性を追加するデータ変数。
     * \throws std::exception 属性の型または値を読み込めない場合。
     */
    void readMissingValueAttributes( VariableInfo& variable ) const;

    /**
     * 値が欠損値または非有限値か判定する。
     *
     * \param[in] variable 欠損値属性を持つ変数。
     * \param[in] value 判定する値。
     * \return 欠損値として扱う場合はtrue。
     */
    bool isMissingValue( const VariableInfo& variable, double value ) const;

    /**
     * MPASの座標・接続・補助変数をデータ変数から除外する。
     *
     * \param[in] name 判定する変数名。
     * \return MPASの構造情報用変数である場合はtrue。
     */
    bool isExcludedVariable( const std::string& name ) const;

    /**
     * 必須変数とmaxLevelCellのメタデータを読み込む。
     *
     * \throws std::exception 必須変数が不足している場合、または構成が不正な場合。
     */
    void readVariableMetadata();

    /**
     * セル中心座標を読み込み、有限値であることを検証する。
     *
     * \throws std::exception 座標を読み込めない場合、または有限値でない場合。
     */
    void readCoordinates();

    /**
     * cellsOnVertexを読み込み、接続先セル番号の範囲を検証する。
     *
     * \throws std::exception 接続情報を読み込めない場合、または範囲外の場合。
     */
    void readConnectivity();

    /**
     * maxLevelCellを読み込み、内部の水平点番号に対応する配列を構築する。
     *
     * \throws std::exception maxLevelCellを読み込めない場合、または値が範囲外の場合。
     */
    void readMaxLevelCell();

    /**
     * on_a_sphere属性を読み込み、球面座標か平面座標かを判定する。
     *
     * \throws std::runtime_error 属性の読み込みに失敗した場合。
     */
    void readSphereAttribute();

    /**
     * データ変数を点データ用とセルデータ用に分類する。
     *
     * \throws std::exception 変数メタデータを取得できない場合。
     */
    void classifyDataVariables();

    /**
     * MPASの水平点番号と鉛直層番号からVTK点IDを生成する。
     *
     * \param[in] horizontal 水平点番号。
     * \param[in] level 鉛直層番号。
     * \return VTK点ID。
     * \throws std::exception IDの計算結果が割り当て範囲外の場合。
     */
    vtkIdType pointTupleId( size_t horizontal, size_t level ) const;

    /**
     * 指定した水平頂点に生成されるセルの有効層数を返す。
     *
     * \param[in] vertex 水平頂点番号。
     * \return 生成されるセルの有効層数。
     * \throws std::exception 接続情報のオフセット計算に失敗した場合。
     */
    size_t cellMaxLevel( size_t vertex ) const;

    /**
     * 点データの欠損値とmaxLevelCellによる無効層を安全な値へ置換する。
     *
     * 返される値は、少なくとも一つの有効サンプルがある場合だけ生成される。
     * 有効サンプルがない場合は空の配列を返し、変数を出力しない。
     *
     * \param[in] variable 対象変数。
     * \param[in] values 読み込んだ点データ。
     * \return VTK点数に対応する置換済み点データ、または空の配列。
     */
    std::vector<double> sanitizePointData(
        const VariableInfo& variable,
        const std::vector<double>& values ) const;

    /**
     * セルデータの欠損値と無効セルを安全な値へ置換する。
     *
     * 返される値は、少なくとも一つの有効サンプルがある場合だけ生成される。
     * 有効サンプルがない場合は空の配列を返し、変数を出力しない。
     *
     * \param[in] variable 対象変数。
     * \param[in] values 読み込んだセルデータ。
     * \return VTKセル数に対応する置換済みセルデータ、または空の配列。
     */
    std::vector<double> sanitizeCellData(
        const VariableInfo& variable,
        const std::vector<double>& values ) const;

    /**
     * セル中心座標と鉛直方向の条件からVTK点座標を生成する。
     *
     * \return 生成した点座標。
     * \throws std::exception 座標の生成に失敗した場合。
     */
    vtkSmartPointer<vtkPoints> createPointCoordinates() const;

    /**
     * cellsOnVertexとmaxLevelCellからVTKセルを生成する。
     *
     * \param[in,out] grid セルを追加する格子。
     * \param[in] cell_type 生成するVTKセル型。
     * \throws std::exception セル生成結果が期待値と一致しない場合。
     */
    void createCells( vtkUnstructuredGrid* grid, int cell_type );

    /**
     * float型の点データ配列を読み込み、格子へ追加する。
     *
     * \param[in,out] grid 配列を追加する格子。
     * \param[in] variable 読み込む変数。
     * \throws std::exception データ読み込みまたは配列生成に失敗した場合。
     */
    void addPointFloatData( vtkUnstructuredGrid* grid, const VariableInfo& variable ) const;

    /**
     * double型の点データ配列を読み込み、格子へ追加する。
     *
     * \param[in,out] grid 配列を追加する格子。
     * \param[in] variable 読み込む変数。
     * \throws std::exception データ読み込みまたは配列生成に失敗した場合。
     */
    void addPointDoubleData( vtkUnstructuredGrid* grid, const VariableInfo& variable ) const;

    /**
     * float型のセルデータ配列を読み込み、格子へ追加する。
     *
     * \param[in,out] grid 配列を追加する格子。
     * \param[in] variable 読み込む変数。
     * \throws std::exception データ読み込みまたは配列生成に失敗した場合。
     */
    void addCellFloatData( vtkUnstructuredGrid* grid, const VariableInfo& variable ) const;

    /**
     * double型のセルデータ配列を読み込み、格子へ追加する。
     *
     * \param[in,out] grid 配列を追加する格子。
     * \param[in] variable 読み込む変数。
     * \throws std::exception データ読み込みまたは配列生成に失敗した場合。
     */
    void addCellDoubleData( vtkUnstructuredGrid* grid, const VariableInfo& variable ) const;

    /**
     * 分類済みのデータ変数を格子へ追加する。
     *
     * \param[in,out] grid 配列を追加する格子。
     * \throws std::exception データ変数が存在しない場合、または読み込みに失敗した場合。
     */
    void addDataArrays( vtkUnstructuredGrid* grid ) const;

    /**
     * セルデータを点データへ変換する。
     *
     * \param[in] grid 変換前の格子。
     * \return 変換後の格子。
     * \throws std::runtime_error VTKの変換結果を取得できない場合。
     */
    vtkSmartPointer<vtkUnstructuredGrid> convertCellDataToPointData(
        vtkUnstructuredGrid* grid ) const;

    /**
     * 最終格子の点数、セル型、データ配列を検証する。
     *
     * \param[in] grid 検証する格子。
     * \param[in] cell_type 期待するVTKセル型。
     * \throws std::runtime_error 最終格子が期待した状態でない場合。
     */
    void validateResult( vtkUnstructuredGrid* grid, int cell_type ) const;

private:
    std::string m_filename;
    int m_layer_thickness = 0;
    bool m_is_atmosphere = false;
    NcFileGuard m_file;

    int m_number_of_dimensions = 0;
    int m_number_of_variables = 0;
    size_t m_number_of_cells = 0;
    size_t m_number_of_vertices = 0;
    size_t m_vertex_degree = 0;
    size_t m_number_of_vertical_levels = 0;
    std::optional<DimensionInfo> m_time_dimension;

    size_t m_vertical_point_count = 0;
    size_t m_horizontal_point_count = 0;
    size_t m_total_point_count = 0;
    size_t m_total_cell_count = 0;
    size_t m_connectivity_value_count = 0;
    size_t m_point_value_count = 0;
    size_t m_cell_value_count = 0;
    size_t m_cell_point_count = 0;
    vtkIdType m_vtk_point_count = 0;
    vtkIdType m_vtk_cell_count = 0;
    vtkIdType m_vtk_cell_point_count = 0;

    VariableInfo m_x_cell_variable;
    VariableInfo m_y_cell_variable;
    VariableInfo m_z_cell_variable;
    VariableInfo m_cells_on_vertex_variable;
    std::optional<VariableInfo> m_max_level_cell_variable;

    std::vector<double> m_x_cell;
    std::vector<double> m_y_cell;
    std::vector<double> m_z_cell;
    std::vector<size_t> m_connections;
    std::vector<size_t> m_max_level_cell;
    std::vector<bool> m_point_has_valid_cell;
    bool m_on_a_sphere = true;

    std::vector<VariableInfo> m_point_variables;
    std::vector<VariableInfo> m_cell_variables;
};
} // namespace ExtendedFileFormat
} // namespace kvs

#endif // EXTENDED_FILE_FORMAT__DIRECT_NETCDF_MPAS_READER_H_INCLUDE
