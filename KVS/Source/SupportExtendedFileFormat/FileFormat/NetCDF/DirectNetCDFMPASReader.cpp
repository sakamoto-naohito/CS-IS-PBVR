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

#include "DirectNetCDFMPASReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <vtkCellData.h>
#include <vtkCellDataToPointData.h>
#include <vtkCell.h>
#include <vtkCellType.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkIdList.h>
#include <vtkFloatArray.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>

namespace kvs
{
namespace ExtendedFileFormat
{
DirectNetCDFMPASReader::DirectNetCDFMPASReader(
    const std::string& filename,
    const int layer_thickness,
    const bool is_atmosphere ):
    m_filename( filename ),
    m_layer_thickness( layer_thickness ),
    m_is_atmosphere( is_atmosphere )
{
    validateInput();
}

void DirectNetCDFMPASReader::NcFileGuard::open( const std::string& path )
{
    filename = path;
    const int status = nc_open( path.c_str(), NC_NOWRITE, &id );
    if ( status != NC_NOERR )
    {
        std::ostringstream message;
        message << "NetCDF error while opening MPAS file '" << filename
                << "' (file): " << nc_strerror( status );
        throw std::runtime_error( message.str() );
    }
}

void DirectNetCDFMPASReader::NcFileGuard::close()
{
    if ( id < 0 )
    {
        return;
    }

    const int status = nc_close( id );
    if ( status != NC_NOERR )
    {
        std::ostringstream message;
        message << "NetCDF error while closing MPAS file '" << filename
                << "' (file): " << nc_strerror( status );
        throw std::runtime_error( message.str() );
    }
    id = -1;
}

DirectNetCDFMPASReader::NcFileGuard::~NcFileGuard()
{
    if ( id >= 0 )
    {
        const int status = nc_close( id );
        if ( status != NC_NOERR )
        {
            std::cerr << "NetCDF error while closing MPAS file '" << filename
                      << "' (file): " << nc_strerror( status ) << std::endl;
        }
    }
}

void DirectNetCDFMPASReader::validateInput() const
{
    if ( m_filename.empty() )
    {
        throw std::invalid_argument( "MPAS data file path is empty." );
    }

    if ( m_layer_thickness <= 0 )
    {
        std::ostringstream message;
        message << "MPAS file '" << m_filename << "' has an invalid layer thickness: "
                << m_layer_thickness << ". It must be positive.";
        throw std::out_of_range( message.str() );
    }
}

void DirectNetCDFMPASReader::checkNetCDFError(
    const int status,
    const std::string& operation,
    const std::string& subject ) const
{
    if ( status != NC_NOERR )
    {
        std::ostringstream message;
        message << "NetCDF error while " << operation << " for MPAS file '" << m_filename
                << "' (" << subject << "): " << nc_strerror( status );
        throw std::runtime_error( message.str() );
    }
}

size_t DirectNetCDFMPASReader::checkedAdd(
    const size_t left,
    const size_t right,
    const std::string& what ) const
{
    if ( right > std::numeric_limits<size_t>::max() - left )
    {
        throw std::overflow_error(
            "MPAS file '" + m_filename + "' has a size_t overflow while calculating " + what + "." );
    }
    return left + right;
}

size_t DirectNetCDFMPASReader::checkedMultiply(
    const size_t left,
    const size_t right,
    const std::string& what ) const
{
    if ( left != 0 && right > std::numeric_limits<size_t>::max() / left )
    {
        throw std::overflow_error(
            "MPAS file '" + m_filename + "' has a size_t overflow while calculating " + what + "." );
    }
    return left * right;
}

vtkIdType DirectNetCDFMPASReader::vtkId(
    const size_t value,
    const std::string& what ) const
{
    if ( static_cast<uintmax_t>( value ) >
         static_cast<uintmax_t>( std::numeric_limits<vtkIdType>::max() ) )
    {
        throw std::overflow_error(
            "MPAS file '" + m_filename +
            "' has a value that cannot be represented by vtkIdType while calculating " + what + "." );
    }
    return static_cast<vtkIdType>( value );
}

void DirectNetCDFMPASReader::readFileMetadata()
{
    int number_of_global_attributes = 0;
    int unlimited_dimension = -1;
    checkNetCDFError(
        nc_inq( m_file.id, &m_number_of_dimensions, &m_number_of_variables,
                &number_of_global_attributes, &unlimited_dimension ),
        "reading file metadata", "file" );
    if ( m_number_of_dimensions < 0 || m_number_of_variables < 0 ||
         number_of_global_attributes < 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' returned invalid file metadata while reading file dimensions and variables." );
    }
    (void) unlimited_dimension;
}

DirectNetCDFMPASReader::DimensionInfo DirectNetCDFMPASReader::readDimension(
    const int dimension_id,
    const std::string& subject ) const
{
    std::array<char, NC_MAX_NAME + 1> dimension_name{};
    size_t dimension_size = 0;
    checkNetCDFError(
        nc_inq_dim( m_file.id, dimension_id, dimension_name.data(), &dimension_size ),
        "reading dimension metadata", subject );
    if ( dimension_size == 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' has a zero-sized dimension '" + subject + "'." );
    }
    return DimensionInfo{ dimension_id, std::string( dimension_name.data() ), dimension_size };
}

DirectNetCDFMPASReader::DimensionInfo DirectNetCDFMPASReader::findRequiredDimension(
    const std::string& dimension_name ) const
{
    int dimension_id = -1;
    checkNetCDFError(
        nc_inq_dimid( m_file.id, dimension_name.c_str(), &dimension_id ),
        "finding required dimension", dimension_name );
    const DimensionInfo dimension = readDimension( dimension_id, dimension_name );
    if ( dimension.name != dimension_name )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' returned dimension '" + dimension.name +
            "' instead of required dimension '" + dimension_name + "'." );
    }
    return dimension;
}

void DirectNetCDFMPASReader::readDimensions()
{
    const DimensionInfo n_cells_dimension = findRequiredDimension( "nCells" );
    const DimensionInfo n_vertices_dimension = findRequiredDimension( "nVertices" );
    const DimensionInfo vertex_degree_dimension = findRequiredDimension( "vertexDegree" );
    const DimensionInfo n_vert_levels_dimension = findRequiredDimension( "nVertLevels" );

    m_number_of_cells = n_cells_dimension.size;
    m_number_of_vertices = n_vertices_dimension.size;
    m_vertex_degree = vertex_degree_dimension.size;
    m_number_of_vertical_levels = n_vert_levels_dimension.size;

    if ( m_vertex_degree != 3 && m_vertex_degree != 4 )
    {
        std::ostringstream message;
        message << "MPAS file '" << m_filename
                << "' has unsupported dimension 'vertexDegree' with size "
                << m_vertex_degree << "; only 3 or 4 is supported.";
        throw std::invalid_argument( message.str() );
    }

    m_time_dimension.reset();
    int time_dimension_id = -1;
    const int time_status = nc_inq_dimid( m_file.id, "Time", &time_dimension_id );
    if ( time_status == NC_NOERR )
    {
        const DimensionInfo dimension = readDimension( time_dimension_id, "Time" );
        if ( dimension.name != "Time" )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename +
                "' returned an unexpected dimension while reading 'Time'." );
        }
        m_time_dimension = dimension;
    }
    else if ( time_status != NC_EBADDIM )
    {
        checkNetCDFError( time_status, "finding optional dimension", "Time" );
    }

    m_vertical_point_count = checkedAdd(
        m_number_of_vertical_levels, 1, "vertical point count" );
    m_horizontal_point_count = checkedAdd(
        m_number_of_cells, 1, "horizontal point count" );
    m_total_point_count = checkedMultiply(
        m_horizontal_point_count, m_vertical_point_count, "total point count" );
    m_total_cell_count = checkedMultiply(
        m_number_of_vertices, m_number_of_vertical_levels, "total cell count" );
    m_connectivity_value_count = checkedMultiply(
        m_number_of_vertices, m_vertex_degree, "cellsOnVertex value count" );
    m_point_value_count = checkedMultiply(
        m_number_of_cells, m_number_of_vertical_levels, "point variable value count" );
    m_cell_value_count = checkedMultiply(
        m_number_of_vertices, m_number_of_vertical_levels, "cell variable value count" );
    m_cell_point_count = checkedMultiply( m_vertex_degree, 2, "cell point count" );

    m_vtk_point_count = vtkId( m_total_point_count, "total point count" );
    m_vtk_cell_count = vtkId( m_total_cell_count, "total cell count" );
    m_vtk_cell_point_count = vtkId( m_cell_point_count, "cell point count" );
}

DirectNetCDFMPASReader::VariableInfo DirectNetCDFMPASReader::readVariable(
    const int variable_id ) const
{
    std::array<char, NC_MAX_NAME + 1> variable_name{};
    nc_type variable_type = NC_NAT;
    int variable_dimension_count = -1;
    std::array<int, NC_MAX_VAR_DIMS> variable_dimension_ids{};
    int number_of_variable_attributes = 0;
    const std::string subject = "variable id " + std::to_string( variable_id );
    checkNetCDFError(
        nc_inq_var( m_file.id, variable_id, variable_name.data(), &variable_type,
                    &variable_dimension_count, variable_dimension_ids.data(),
                    &number_of_variable_attributes ),
        "reading variable metadata", subject );
    if ( variable_dimension_count < 0 || variable_dimension_count > NC_MAX_VAR_DIMS )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' returned an invalid dimension count for variable '" +
            std::string( variable_name.data() ) + "'." );
    }
    (void) number_of_variable_attributes;

    return VariableInfo{
        variable_id,
        std::string( variable_name.data() ),
        variable_type,
        std::vector<int>( variable_dimension_ids.begin(),
                          variable_dimension_ids.begin() + variable_dimension_count ) };
}

DirectNetCDFMPASReader::VariableInfo DirectNetCDFMPASReader::findRequiredVariable(
    const std::string& variable_name ) const
{
    int variable_id = -1;
    checkNetCDFError(
        nc_inq_varid( m_file.id, variable_name.c_str(), &variable_id ),
        "finding required variable", variable_name );
    const VariableInfo variable = readVariable( variable_id );
    if ( variable.name != variable_name )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' returned variable '" + variable.name +
            "' instead of required variable '" + variable_name + "'." );
    }
    return variable;
}

void DirectNetCDFMPASReader::validateVariableDimensions(
    const VariableInfo& variable,
    const std::vector<std::string>& expected_names,
    const std::vector<size_t>& expected_sizes ) const
{
    if ( expected_names.size() != expected_sizes.size() ||
         variable.dimension_ids.size() != expected_names.size() )
    {
        std::ostringstream message;
        message << "MPAS file '" << m_filename << "' variable '" << variable.name
                << "' has " << variable.dimension_ids.size()
                << " dimensions; expected " << expected_names.size()
                << " dimensions in the required order.";
        throw std::invalid_argument( message.str() );
    }

    for ( size_t index = 0; index < expected_names.size(); ++index )
    {
        const DimensionInfo dimension = readDimension(
            variable.dimension_ids[index],
            variable.name + " dimension " + std::to_string( index ) );
        if ( dimension.name != expected_names[index] ||
             dimension.size != expected_sizes[index] )
        {
            std::ostringstream message;
            message << "MPAS file '" << m_filename << "' variable '" << variable.name
                    << "' has dimension " << index << " ('" << dimension.name
                    << "', size " << dimension.size << "), expected ('"
                    << expected_names[index] << "', size " << expected_sizes[index] << ").";
            throw std::invalid_argument( message.str() );
        }
    }
}

bool DirectNetCDFMPASReader::hasVariableDimensions(
    const VariableInfo& variable,
    const std::vector<std::string>& expected_names,
    const std::vector<size_t>& expected_sizes ) const
{
    if ( expected_names.size() != expected_sizes.size() ||
         variable.dimension_ids.size() != expected_names.size() )
    {
        return false;
    }

    for ( size_t index = 0; index < expected_names.size(); ++index )
    {
        const DimensionInfo dimension = readDimension(
            variable.dimension_ids[index],
            variable.name + " dimension " + std::to_string( index ) );
        if ( dimension.name != expected_names[index] ||
             dimension.size != expected_sizes[index] )
        {
            return false;
        }
    }
    return true;
}

bool DirectNetCDFMPASReader::isIntegerType( const nc_type type )
{
    switch ( type )
    {
    case NC_BYTE:
    case NC_SHORT:
    case NC_INT:
    case NC_UBYTE:
    case NC_USHORT:
    case NC_UINT:
    case NC_INT64:
    case NC_UINT64:
        return true;
    default:
        return false;
    }
}

bool DirectNetCDFMPASReader::isNumericType( const nc_type type )
{
    return isIntegerType( type ) || type == NC_FLOAT || type == NC_DOUBLE;
}

void DirectNetCDFMPASReader::readMissingValueAttributes( VariableInfo& variable ) const
{
    variable.missing_values.clear();

    const std::array<const char*, 2> attribute_names = { "_FillValue", "missing_value" };
    for ( const char* attribute_name : attribute_names )
    {
        nc_type attribute_type = NC_NAT;
        size_t attribute_length = 0;
        const int attribute_status = nc_inq_att(
            m_file.id, variable.id, attribute_name, &attribute_type, &attribute_length );
        if ( attribute_status == NC_ENOTATT )
        {
            continue;
        }
        checkNetCDFError(
            attribute_status, "finding variable attribute", variable.name + ":" + attribute_name );

        if ( !isNumericType( attribute_type ) || attribute_length == 0 )
        {
            throw std::invalid_argument(
                "MPAS file '" + m_filename + "' variable '" + variable.name +
                "' has an invalid numeric " + attribute_name + " attribute." );
        }
        if ( std::string( attribute_name ) == "_FillValue" && attribute_length != 1 )
        {
            throw std::invalid_argument(
                "MPAS file '" + m_filename + "' variable '" + variable.name +
                "' has a non-scalar _FillValue attribute." );
        }
        if ( attribute_length > std::vector<double>().max_size() )
        {
            throw std::overflow_error(
                "MPAS file '" + m_filename + "' variable '" + variable.name +
                "' has an attribute that is too large to read." );
        }

        std::vector<double> attribute_values( attribute_length );
        checkNetCDFError(
            nc_get_att_double(
                m_file.id, variable.id, attribute_name, attribute_values.data() ),
            "reading variable attribute", variable.name + ":" + attribute_name );
        variable.missing_values.insert(
            variable.missing_values.end(), attribute_values.begin(), attribute_values.end() );
    }
}

bool DirectNetCDFMPASReader::isMissingValue(
    const VariableInfo& variable,
    const double value ) const
{
    if ( !std::isfinite( value ) )
    {
        return true;
    }
    return std::any_of(
        variable.missing_values.begin(), variable.missing_values.end(),
        [value]( const double missing_value )
        {
            return value == missing_value;
        } );
}

void DirectNetCDFMPASReader::readVariableMetadata()
{
    m_x_cell_variable = findRequiredVariable( "xCell" );
    m_y_cell_variable = findRequiredVariable( "yCell" );
    m_z_cell_variable = findRequiredVariable( "zCell" );
    m_cells_on_vertex_variable = findRequiredVariable( "cellsOnVertex" );

    validateVariableDimensions(
        m_x_cell_variable, { "nCells" }, { m_number_of_cells } );
    validateVariableDimensions(
        m_y_cell_variable, { "nCells" }, { m_number_of_cells } );
    validateVariableDimensions(
        m_z_cell_variable, { "nCells" }, { m_number_of_cells } );
    validateVariableDimensions(
        m_cells_on_vertex_variable, { "nVertices", "vertexDegree" },
        { m_number_of_vertices, m_vertex_degree } );

    if ( !isNumericType( m_x_cell_variable.type ) )
    {
        throw std::invalid_argument(
            "MPAS file '" + m_filename + "' coordinate variable 'xCell' is not numeric." );
    }
    if ( !isNumericType( m_y_cell_variable.type ) )
    {
        throw std::invalid_argument(
            "MPAS file '" + m_filename + "' coordinate variable 'yCell' is not numeric." );
    }
    if ( !isNumericType( m_z_cell_variable.type ) )
    {
        throw std::invalid_argument(
            "MPAS file '" + m_filename + "' coordinate variable 'zCell' is not numeric." );
    }
    if ( !isIntegerType( m_cells_on_vertex_variable.type ) )
    {
        throw std::invalid_argument(
            "MPAS file '" + m_filename +
            "' connectivity variable 'cellsOnVertex' is not an integer type." );
    }

    m_max_level_cell_variable.reset();
    int max_level_cell_id = -1;
    const int max_level_status =
        nc_inq_varid( m_file.id, "maxLevelCell", &max_level_cell_id );
    if ( max_level_status == NC_NOERR )
    {
        const VariableInfo variable = readVariable( max_level_cell_id );
        if ( variable.name != "maxLevelCell" )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename + "' returned variable '" + variable.name +
                "' instead of optional variable 'maxLevelCell'." );
        }
        validateVariableDimensions( variable, { "nCells" }, { m_number_of_cells } );
        if ( !isIntegerType( variable.type ) )
        {
            throw std::invalid_argument(
                "MPAS file '" + m_filename +
                "' optional variable 'maxLevelCell' is not an integer type." );
        }
        m_max_level_cell_variable = variable;
    }
    else if ( max_level_status != NC_ENOTVAR )
    {
        checkNetCDFError( max_level_status, "finding optional variable", "maxLevelCell" );
    }
}

void DirectNetCDFMPASReader::readCoordinates()
{
    m_x_cell.assign( m_number_of_cells, 0.0 );
    m_y_cell.assign( m_number_of_cells, 0.0 );
    m_z_cell.assign( m_number_of_cells, 0.0 );
    checkNetCDFError(
        nc_get_var_double( m_file.id, m_x_cell_variable.id, m_x_cell.data() ),
        "reading coordinate variable", "xCell" );
    checkNetCDFError(
        nc_get_var_double( m_file.id, m_y_cell_variable.id, m_y_cell.data() ),
        "reading coordinate variable", "yCell" );
    checkNetCDFError(
        nc_get_var_double( m_file.id, m_z_cell_variable.id, m_z_cell.data() ),
        "reading coordinate variable", "zCell" );

    for ( size_t cell = 0; cell < m_number_of_cells; ++cell )
    {
        if ( !std::isfinite( m_x_cell[cell] ) || !std::isfinite( m_y_cell[cell] ) ||
             !std::isfinite( m_z_cell[cell] ) )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename +
                "' has a non-finite value in coordinate variables xCell/yCell/zCell at cell " +
                std::to_string( cell ) + "." );
        }
    }
}

void DirectNetCDFMPASReader::readConnectivity()
{
    std::vector<long long> raw_connections( m_connectivity_value_count );
    checkNetCDFError(
        nc_get_var_longlong(
            m_file.id, m_cells_on_vertex_variable.id, raw_connections.data() ),
        "reading connectivity variable", "cellsOnVertex" );

    m_connections.resize( m_connectivity_value_count );
    for ( size_t index = 0; index < m_connectivity_value_count; ++index )
    {
        const long long connection = raw_connections[index];
        const bool in_range =
            connection >= 0 &&
            static_cast<uintmax_t>( connection ) <= static_cast<uintmax_t>( m_number_of_cells );
        if ( !in_range )
        {
            const size_t vertex = index / m_vertex_degree;
            const size_t local_index = index % m_vertex_degree;
            std::ostringstream message;
            message << "MPAS file '" << m_filename
                    << "' connectivity variable 'cellsOnVertex' has out-of-range connection "
                    << connection << " at vertex " << vertex << ", index " << local_index
                    << "; expected 0 <= connection <= " << m_number_of_cells << ".";
            throw std::out_of_range( message.str() );
        }
        m_connections[index] = static_cast<size_t>( connection );
    }
}

void DirectNetCDFMPASReader::readMaxLevelCell()
{
    m_max_level_cell.assign( m_horizontal_point_count, m_number_of_vertical_levels );
    // cellsOnVertexの0は欠損接続を表すため、予約した水平点0は有効層を持たない。
    m_max_level_cell[0] = 0;
    if ( !m_max_level_cell_variable )
    {
        return;
    }

    std::vector<long long> raw_max_level_cell( m_number_of_cells );
    checkNetCDFError(
        nc_get_var_longlong(
            m_file.id, m_max_level_cell_variable->id, raw_max_level_cell.data() ),
        "reading optional variable", "maxLevelCell" );
    for ( size_t cell = 0; cell < m_number_of_cells; ++cell )
    {
        const long long maximum_level = raw_max_level_cell[cell];
        const bool in_range =
            maximum_level >= 0 &&
            static_cast<uintmax_t>( maximum_level ) <=
                static_cast<uintmax_t>( m_number_of_vertical_levels );
        if ( !in_range )
        {
            std::ostringstream message;
            message << "MPAS file '" << m_filename
                    << "' optional variable 'maxLevelCell' has out-of-range value "
                    << maximum_level << " at index " << cell << "; expected 0 <= value <= "
                    << m_number_of_vertical_levels << ".";
            throw std::out_of_range( message.str() );
        }
        m_max_level_cell[checkedAdd( cell, 1, "maxLevelCell internal index" )] =
            static_cast<size_t>( maximum_level );
    }
}

void DirectNetCDFMPASReader::readSphereAttribute()
{
    m_on_a_sphere = true;
    nc_type sphere_attribute_type = NC_NAT;
    size_t sphere_attribute_length = 0;
    const int sphere_attribute_status = nc_inq_att(
        m_file.id, NC_GLOBAL, "on_a_sphere", &sphere_attribute_type,
        &sphere_attribute_length );
    if ( sphere_attribute_status == NC_NOERR )
    {
        if ( sphere_attribute_type == NC_CHAR )
        {
            const size_t attribute_buffer_size =
                checkedAdd( sphere_attribute_length, 1, "on_a_sphere attribute buffer" );
            std::vector<char> attribute( attribute_buffer_size, '\0' );
            checkNetCDFError(
                nc_get_att_text( m_file.id, NC_GLOBAL, "on_a_sphere", attribute.data() ),
                "reading global attribute", "on_a_sphere" );
            m_on_a_sphere = sphere_attribute_length == 3 && attribute[0] == 'Y' &&
                            attribute[1] == 'E' && attribute[2] == 'S';
        }
        else
        {
            m_on_a_sphere = false;
        }
    }
    else if ( sphere_attribute_status != NC_ENOTATT )
    {
        checkNetCDFError( sphere_attribute_status, "finding global attribute", "on_a_sphere" );
    }
}

bool DirectNetCDFMPASReader::isExcludedVariable( const std::string& name ) const
{
    return name == "xCell" || name == "yCell" || name == "zCell" ||
           name == "cellsOnVertex" || name == "maxLevelCell";
}

void DirectNetCDFMPASReader::classifyDataVariables()
{
    m_point_variables.clear();
    m_cell_variables.clear();
    const std::vector<std::string> point_dimensions = { "nCells", "nVertLevels" };
    const std::vector<std::string> cell_dimensions = { "nVertices", "nVertLevels" };
    const std::vector<size_t> point_dimension_sizes = {
        m_number_of_cells, m_number_of_vertical_levels };
    const std::vector<size_t> cell_dimension_sizes = {
        m_number_of_vertices, m_number_of_vertical_levels };
    std::vector<std::string> point_time_dimensions;
    std::vector<std::string> cell_time_dimensions;
    std::vector<size_t> point_time_dimension_sizes;
    std::vector<size_t> cell_time_dimension_sizes;
    if ( m_time_dimension )
    {
        point_time_dimensions = { "Time", "nCells", "nVertLevels" };
        cell_time_dimensions = { "Time", "nVertices", "nVertLevels" };
        point_time_dimension_sizes = {
            m_time_dimension->size, m_number_of_cells, m_number_of_vertical_levels };
        cell_time_dimension_sizes = {
            m_time_dimension->size, m_number_of_vertices, m_number_of_vertical_levels };
    }

    for ( int variable_id = 0; variable_id < m_number_of_variables; ++variable_id )
    {
        const VariableInfo variable = readVariable( variable_id );
        if ( isExcludedVariable( variable.name ) ||
             ( variable.type != NC_FLOAT && variable.type != NC_DOUBLE ) )
        {
            continue;
        }

        if ( hasVariableDimensions( variable, point_dimensions, point_dimension_sizes ) ||
             ( !point_time_dimensions.empty() &&
               hasVariableDimensions( variable, point_time_dimensions, point_time_dimension_sizes ) ) )
        {
            VariableInfo data_variable = variable;
            readMissingValueAttributes( data_variable );
            m_point_variables.push_back( data_variable );
            continue;
        }

        if ( hasVariableDimensions( variable, cell_dimensions, cell_dimension_sizes ) ||
             ( !cell_time_dimensions.empty() &&
               hasVariableDimensions( variable, cell_time_dimensions, cell_time_dimension_sizes ) ) )
        {
            VariableInfo data_variable = variable;
            readMissingValueAttributes( data_variable );
            m_cell_variables.push_back( data_variable );
        }
    }

    if ( m_point_variables.empty() && m_cell_variables.empty() )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' contains no NC_FLOAT or NC_DOUBLE data variable with a supported dimension layout." );
    }
}

vtkIdType DirectNetCDFMPASReader::pointTupleId(
    const size_t horizontal,
    const size_t level ) const
{
    const size_t offset = checkedMultiply( horizontal, m_vertical_point_count, "point id" );
    const size_t value = checkedAdd( offset, level, "point id" );
    if ( value >= m_total_point_count )
    {
        throw std::out_of_range(
            "MPAS file '" + m_filename +
            "' generated a point id outside the allocated point range." );
    }
    return vtkId( value, "point id" );
}

size_t DirectNetCDFMPASReader::cellMaxLevel( const size_t vertex ) const
{
    if ( vertex >= m_number_of_vertices )
    {
        throw std::out_of_range(
            "MPAS file '" + m_filename + "' generated an out-of-range horizontal vertex index." );
    }

    if ( !m_max_level_cell_variable )
    {
        return m_number_of_vertical_levels;
    }

    const size_t connection_offset =
        checkedMultiply( vertex, m_vertex_degree, "cellsOnVertex row offset" );
    size_t maximum_level = m_number_of_vertical_levels;
    for ( size_t local_index = 0; local_index < m_vertex_degree; ++local_index )
    {
        const size_t connection_index = checkedAdd(
            connection_offset, local_index, "cellsOnVertex value index" );
        maximum_level = std::min(
            maximum_level,
            m_max_level_cell[m_connections[connection_index]] );
    }
    return maximum_level;
}

std::vector<double> DirectNetCDFMPASReader::sanitizePointData(
    const VariableInfo& variable,
    const std::vector<double>& values ) const
{
    if ( values.size() != m_point_value_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' read an unexpected number of values for point " +
            "variable '" + variable.name + "'." );
    }

    std::vector<std::optional<double>> column_fallback( m_number_of_cells );
    std::optional<double> global_fallback;

    for ( size_t source_horizontal = 0;
          source_horizontal < m_number_of_cells; ++source_horizontal )
    {
        const size_t valid_level_count = m_max_level_cell_variable
                                              ? m_max_level_cell[checkedAdd(
                                                    source_horizontal, 1,
                                                    "point data maxLevelCell index" )]
                                              : m_number_of_vertical_levels;
        const size_t source_offset = checkedMultiply(
            source_horizontal, m_number_of_vertical_levels, "point data source offset" );
        for ( size_t level = 0; level < valid_level_count; ++level )
        {
            if ( m_max_level_cell_variable &&
                 !m_point_has_valid_cell[static_cast<size_t>( pointTupleId(
                     checkedAdd( source_horizontal, 1, "point data horizontal index" ),
                     level ) )] )
            {
                continue;
            }
            const size_t source_index = checkedAdd(
                source_offset, level, "point data source index" );
            if ( isMissingValue( variable, values[source_index] ) )
            {
                continue;
            }

            column_fallback[source_horizontal] = values[source_index];
            if ( !global_fallback )
            {
                global_fallback = values[source_index];
            }
        }
    }

    // 無効層しか存在しない変数を有限値で埋めると、その値だけで値域を作ってしまう。
    // その場合は変数を出力せず、PFI/KVSの値域へ影響させない。
    if ( !global_fallback )
    {
        return {};
    }

    std::vector<double> sanitized( m_total_point_count, *global_fallback );
    for ( size_t horizontal = 0; horizontal < m_horizontal_point_count; ++horizontal )
    {
        const bool is_reserved_point = horizontal == 0;
        const size_t source_horizontal = is_reserved_point ? 0 : horizontal - 1;
        const size_t valid_level_count =
            m_max_level_cell_variable
                ? m_max_level_cell[is_reserved_point
                                       ? 0
                                       : checkedAdd( source_horizontal, 1,
                                                     "point data maxLevelCell index" )]
                : m_number_of_vertical_levels;

        for ( size_t level = 0; level < m_vertical_point_count; ++level )
        {
            double value = *global_fallback;
            if ( valid_level_count != 0 &&
                 ( !m_max_level_cell_variable || !is_reserved_point ) )
            {
                const size_t source_level = m_max_level_cell_variable
                                                ? std::min( level, valid_level_count - 1 )
                                                : std::min(
                                                      level, m_number_of_vertical_levels - 1 );
                const size_t source_offset = checkedMultiply(
                    source_horizontal, m_number_of_vertical_levels,
                    "point data source offset" );
                const size_t source_index = checkedAdd(
                    source_offset, source_level, "point data source index" );
                if ( !isMissingValue( variable, values[source_index] ) )
                {
                    value = values[source_index];
                }
                else if ( column_fallback[source_horizontal] )
                {
                    value = *column_fallback[source_horizontal];
                }
            }

            sanitized[static_cast<size_t>( pointTupleId( horizontal, level ) )] = value;
        }
    }
    return sanitized;
}

std::vector<double> DirectNetCDFMPASReader::sanitizeCellData(
    const VariableInfo& variable,
    const std::vector<double>& values ) const
{
    if ( values.size() != m_cell_value_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' read an unexpected number of values for cell " +
            "variable '" + variable.name + "'." );
    }

    std::vector<std::optional<double>> vertex_fallback( m_number_of_vertices );
    std::optional<double> global_fallback;
    for ( size_t vertex = 0; vertex < m_number_of_vertices; ++vertex )
    {
        const size_t valid_level_count = cellMaxLevel( vertex );
        const size_t source_offset = checkedMultiply(
            vertex, m_number_of_vertical_levels, "cell data source offset" );
        for ( size_t level = 0; level < valid_level_count; ++level )
        {
            const size_t source_index = checkedAdd(
                source_offset, level, "cell data source index" );
            if ( isMissingValue( variable, values[source_index] ) )
            {
                continue;
            }

            vertex_fallback[vertex] = values[source_index];
            if ( !global_fallback )
            {
                global_fallback = values[source_index];
            }
        }
    }

    // 無効セルしか存在しない変数は、退化セル用の代替値を作らず出力しない。
    if ( !global_fallback )
    {
        return {};
    }

    std::vector<double> sanitized( m_total_cell_count, *global_fallback );
    for ( size_t vertex = 0; vertex < m_number_of_vertices; ++vertex )
    {
        const size_t valid_level_count = cellMaxLevel( vertex );
        const size_t source_offset = checkedMultiply(
            vertex, m_number_of_vertical_levels, "cell data source offset" );
        for ( size_t level = 0; level < m_number_of_vertical_levels; ++level )
        {
            const size_t source_index = checkedAdd(
                source_offset, level, "cell data source index" );
            double value = *global_fallback;
            if ( level < valid_level_count &&
                 !isMissingValue( variable, values[source_index] ) )
            {
                value = values[source_index];
            }
            else if ( vertex_fallback[vertex] )
            {
                value = *vertex_fallback[vertex];
            }
            sanitized[source_index] = value;
        }
    }
    return sanitized;
}

vtkSmartPointer<vtkPoints> DirectNetCDFMPASReader::createPointCoordinates() const
{
    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    points->SetNumberOfPoints( m_vtk_point_count );

    // 接続番号0を欠損接続として安全に参照できるよう、水平点0を原点として予約する。
    const double layer_thickness = static_cast<double>( m_layer_thickness );
    for ( size_t horizontal = 0; horizontal < m_horizontal_point_count; ++horizontal )
    {
        double original_x = 0.0;
        double original_y = 0.0;
        double original_z = 0.0;
        double radius = 0.0;
        if ( horizontal != 0 )
        {
            const size_t cell = horizontal - 1;
            original_x = m_x_cell[cell];
            original_y = m_y_cell[cell];
            original_z = m_z_cell[cell];
            if ( m_on_a_sphere )
            {
                radius = std::sqrt(
                    original_x * original_x + original_y * original_y + original_z * original_z );
                if ( !std::isfinite( radius ) || radius == 0.0 )
                {
                    throw std::runtime_error(
                        "MPAS file '" + m_filename + "' has an invalid spherical radius at cell " +
                        std::to_string( cell ) + "; the radius must be finite and non-zero." );
                }
            }
        }

        for ( size_t level = 0; level < m_vertical_point_count; ++level )
        {
            double point_x = 0.0;
            double point_y = 0.0;
            double point_z = 0.0;
            if ( horizontal != 0 )
            {
                const double level_as_double = static_cast<double>( level );
                if ( !std::isfinite( level_as_double ) ||
                     level_as_double > std::numeric_limits<double>::max() / layer_thickness )
                {
                    if ( m_on_a_sphere )
                    {
                        throw std::overflow_error(
                            "MPAS file '" + m_filename +
                            "' cannot represent the spherical layer offset for cell " +
                            std::to_string( horizontal - 1 ) + "." );
                    }
                    throw std::overflow_error(
                        "MPAS file '" + m_filename +
                        "' cannot represent the planar layer offset for cell " +
                        std::to_string( horizontal - 1 ) + "." );
                }

                if ( m_on_a_sphere )
                {
                    const double level_radius =
                        m_is_atmosphere ? radius + layer_thickness * level_as_double
                                        : radius - layer_thickness * level_as_double;
                    if ( !std::isfinite( level_radius ) ||
                         ( !m_is_atmosphere && level_radius <= 0.0 ) )
                    {
                        throw std::runtime_error(
                            "MPAS file '" + m_filename +
                            "' has an invalid spherical level radius at cell " +
                            std::to_string( horizontal - 1 ) + ", level " +
                            std::to_string( level ) + "." );
                    }
                    const double scale = level_radius / radius;
                    point_x = original_x * scale;
                    point_y = original_y * scale;
                    point_z = original_z * scale;
                }
                else
                {
                    point_x = original_x;
                    point_y = original_y;
                    point_z = m_is_atmosphere ? layer_thickness * level_as_double
                                              : -layer_thickness * level_as_double;
                }
            }

            if ( !std::isfinite( point_x ) || !std::isfinite( point_y ) ||
                 !std::isfinite( point_z ) )
            {
                throw std::runtime_error(
                    "MPAS file '" + m_filename +
                    "' generated a non-finite point coordinate at horizontal point " +
                    std::to_string( horizontal ) + ", level " + std::to_string( level ) + "." );
            }
            points->SetPoint( pointTupleId( horizontal, level ), point_x, point_y, point_z );
        }
    }

    if ( points->GetNumberOfPoints() != m_vtk_point_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' generated an unexpected number of points." );
    }
    return points;
}

void DirectNetCDFMPASReader::createCells(
    vtkUnstructuredGrid* grid,
    const int cell_type )
{
    // MPASの接続番号は水平点を参照するため、各水平点の上下層を結んで柱状セルを作る。
    std::vector<vtkIdType> cell_point_ids( m_cell_point_count );
    m_point_has_valid_cell.assign( m_total_point_count, false );
    size_t generated_cell_count = 0;
    for ( size_t vertex = 0; vertex < m_number_of_vertices; ++vertex )
    {
        const size_t connection_offset =
            checkedMultiply( vertex, m_vertex_degree, "cellsOnVertex row offset" );
        const size_t cell_max_level = cellMaxLevel( vertex );

        for ( size_t level = 0; level < m_number_of_vertical_levels; ++level )
        {
            const bool valid_cell = !m_max_level_cell_variable || level < cell_max_level;
            if ( !valid_cell )
            {
                // maxLevelCellで存在しない層もセル数を維持するため、予約点0による退化セルにする。
                std::fill(
                    cell_point_ids.begin(), cell_point_ids.end(),
                    static_cast<vtkIdType>( 0 ) );
            }
            else
            {
                for ( size_t local_index = 0; local_index < m_vertex_degree; ++local_index )
                {
                    const size_t connection =
                        m_connections[checkedAdd(
                            connection_offset, local_index, "cellsOnVertex value index" )];
                    cell_point_ids[local_index] = pointTupleId( connection, level );
                    cell_point_ids[m_vertex_degree + local_index] = pointTupleId(
                        connection, checkedAdd( level, 1, "upper cell point level" ) );
                    m_point_has_valid_cell[static_cast<size_t>(
                        cell_point_ids[local_index] )] = true;
                    m_point_has_valid_cell[static_cast<size_t>(
                        cell_point_ids[m_vertex_degree + local_index] )] = true;
                }
            }

            const vtkIdType inserted_cell = grid->InsertNextCell(
                cell_type, m_vtk_cell_point_count, cell_point_ids.data() );
            if ( inserted_cell != vtkId( generated_cell_count, "generated cell id" ) )
            {
                throw std::runtime_error(
                    "MPAS file '" + m_filename + "' generated a cell id out of order." );
            }
            generated_cell_count = checkedAdd( generated_cell_count, 1, "generated cell count" );
        }
    }

    if ( generated_cell_count != m_total_cell_count ||
         grid->GetNumberOfCells() != m_vtk_cell_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' generated an unexpected number of cells." );
    }
    if ( grid->GetNumberOfPoints() == 0 || grid->GetNumberOfCells() == 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename + "' generated an empty vtkUnstructuredGrid." );
    }
    for ( vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell )
    {
        if ( grid->GetCellType( cell ) != cell_type )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename + "' generated mixed or unexpected cell types." );
        }
    }
}

void DirectNetCDFMPASReader::addPointFloatData(
    vtkUnstructuredGrid* grid,
    const VariableInfo& variable ) const
{
    std::vector<float> values( m_point_value_count );
    if ( variable.dimension_ids.size() == 3 )
    {
        size_t start[3] = { 0, 0, 0 };
        size_t count[3] = { 1, m_number_of_cells, m_number_of_vertical_levels };
        checkNetCDFError(
            nc_get_vara_float( m_file.id, variable.id, start, count, values.data() ),
            "reading time-index 0 data variable", variable.name );
    }
    else
    {
        checkNetCDFError(
            nc_get_var_float( m_file.id, variable.id, values.data() ),
            "reading data variable", variable.name );
    }

    std::vector<double> double_values( values.size() );
    std::transform(
        values.begin(), values.end(), double_values.begin(),
        []( const float value ) { return static_cast<double>( value ); } );
    const std::vector<double> sanitized = sanitizePointData( variable, double_values );
    if ( sanitized.empty() )
    {
        return;
    }

    vtkNew<vtkFloatArray> array;
    array->SetName( variable.name.c_str() );
    array->SetNumberOfComponents( 1 );
    array->SetNumberOfTuples( m_vtk_point_count );
    for ( size_t horizontal = 0; horizontal < m_horizontal_point_count; ++horizontal )
    {
        for ( size_t level = 0; level < m_vertical_point_count; ++level )
        {
            const vtkIdType point_id = pointTupleId( horizontal, level );
            array->SetValue(
                point_id, static_cast<float>( sanitized[static_cast<size_t>( point_id )] ) );
        }
    }
    if ( array->GetNumberOfTuples() != m_vtk_point_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' created an unexpected tuple count for point variable '" + variable.name + "'." );
    }
    grid->GetPointData()->AddArray( array );
}

void DirectNetCDFMPASReader::addPointDoubleData(
    vtkUnstructuredGrid* grid,
    const VariableInfo& variable ) const
{
    std::vector<double> values( m_point_value_count );
    if ( variable.dimension_ids.size() == 3 )
    {
        size_t start[3] = { 0, 0, 0 };
        size_t count[3] = { 1, m_number_of_cells, m_number_of_vertical_levels };
        checkNetCDFError(
            nc_get_vara_double( m_file.id, variable.id, start, count, values.data() ),
            "reading time-index 0 data variable", variable.name );
    }
    else
    {
        checkNetCDFError(
            nc_get_var_double( m_file.id, variable.id, values.data() ),
            "reading data variable", variable.name );
    }

    const std::vector<double> sanitized = sanitizePointData( variable, values );
    if ( sanitized.empty() )
    {
        return;
    }

    vtkNew<vtkDoubleArray> array;
    array->SetName( variable.name.c_str() );
    array->SetNumberOfComponents( 1 );
    array->SetNumberOfTuples( m_vtk_point_count );
    for ( size_t horizontal = 0; horizontal < m_horizontal_point_count; ++horizontal )
    {
        for ( size_t level = 0; level < m_vertical_point_count; ++level )
        {
            const vtkIdType point_id = pointTupleId( horizontal, level );
            array->SetValue( point_id, sanitized[static_cast<size_t>( point_id )] );
        }
    }
    if ( array->GetNumberOfTuples() != m_vtk_point_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' created an unexpected tuple count for point variable '" + variable.name + "'." );
    }
    grid->GetPointData()->AddArray( array );
}

void DirectNetCDFMPASReader::addCellFloatData(
    vtkUnstructuredGrid* grid,
    const VariableInfo& variable ) const
{
    std::vector<float> values( m_cell_value_count );
    if ( variable.dimension_ids.size() == 3 )
    {
        size_t start[3] = { 0, 0, 0 };
        size_t count[3] = { 1, m_number_of_vertices, m_number_of_vertical_levels };
        checkNetCDFError(
            nc_get_vara_float( m_file.id, variable.id, start, count, values.data() ),
            "reading time-index 0 data variable", variable.name );
    }
    else
    {
        checkNetCDFError(
            nc_get_var_float( m_file.id, variable.id, values.data() ),
            "reading data variable", variable.name );
    }

    std::vector<double> double_values( values.size() );
    std::transform(
        values.begin(), values.end(), double_values.begin(),
        []( const float value ) { return static_cast<double>( value ); } );
    const std::vector<double> sanitized = sanitizeCellData( variable, double_values );
    if ( sanitized.empty() )
    {
        return;
    }

    vtkNew<vtkFloatArray> array;
    array->SetName( variable.name.c_str() );
    array->SetNumberOfComponents( 1 );
    array->SetNumberOfTuples( m_vtk_cell_count );
    for ( size_t index = 0; index < m_cell_value_count; ++index )
    {
        array->SetValue(
            vtkId( index, "cell data tuple id" ), static_cast<float>( sanitized[index] ) );
    }
    if ( array->GetNumberOfTuples() != m_vtk_cell_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' created an unexpected tuple count for cell variable '" + variable.name + "'." );
    }
    grid->GetCellData()->AddArray( array );
}

void DirectNetCDFMPASReader::addCellDoubleData(
    vtkUnstructuredGrid* grid,
    const VariableInfo& variable ) const
{
    std::vector<double> values( m_cell_value_count );
    if ( variable.dimension_ids.size() == 3 )
    {
        size_t start[3] = { 0, 0, 0 };
        size_t count[3] = { 1, m_number_of_vertices, m_number_of_vertical_levels };
        checkNetCDFError(
            nc_get_vara_double( m_file.id, variable.id, start, count, values.data() ),
            "reading time-index 0 data variable", variable.name );
    }
    else
    {
        checkNetCDFError(
            nc_get_var_double( m_file.id, variable.id, values.data() ),
            "reading data variable", variable.name );
    }

    const std::vector<double> sanitized = sanitizeCellData( variable, values );
    if ( sanitized.empty() )
    {
        return;
    }

    vtkNew<vtkDoubleArray> array;
    array->SetName( variable.name.c_str() );
    array->SetNumberOfComponents( 1 );
    array->SetNumberOfTuples( m_vtk_cell_count );
    for ( size_t index = 0; index < m_cell_value_count; ++index )
    {
        array->SetValue( vtkId( index, "cell data tuple id" ), sanitized[index] );
    }
    if ( array->GetNumberOfTuples() != m_vtk_cell_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' created an unexpected tuple count for cell variable '" + variable.name + "'." );
    }
    grid->GetCellData()->AddArray( array );
}

void DirectNetCDFMPASReader::addDataArrays( vtkUnstructuredGrid* grid ) const
{
    // Time次元付き変数は各型別関数で先頭スライスだけを読み、従来どおり先頭時刻を使う。
    for ( const VariableInfo& variable : m_point_variables )
    {
        if ( variable.type == NC_FLOAT )
        {
            addPointFloatData( grid, variable );
        }
        else
        {
            addPointDoubleData( grid, variable );
        }
    }
    for ( const VariableInfo& variable : m_cell_variables )
    {
        if ( variable.type == NC_FLOAT )
        {
            addCellFloatData( grid, variable );
        }
        else
        {
            addCellDoubleData( grid, variable );
        }
    }

    if ( grid->GetPointData()->GetNumberOfArrays() == 0 &&
         grid->GetCellData()->GetNumberOfArrays() == 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' produced no floating-point point or cell data arrays." );
    }
}

vtkSmartPointer<vtkUnstructuredGrid> DirectNetCDFMPASReader::convertCellDataToPointData(
    vtkUnstructuredGrid* grid ) const
{
    // MPASではセル中心に格納された変数もあるため、最終出力を点データへ統一する。
    vtkNew<vtkCellDataToPointData> cell_data_to_point_data;
    cell_data_to_point_data->PassCellDataOff();
    cell_data_to_point_data->SetInputData( grid );
    cell_data_to_point_data->Update();

    vtkUnstructuredGrid* converted_grid = vtkUnstructuredGrid::SafeDownCast(
        cell_data_to_point_data->GetOutputDataObject( 0 ) );
    if ( !converted_grid )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' failed to produce a vtkUnstructuredGrid after cell-to-point conversion." );
    }

    vtkSmartPointer<vtkUnstructuredGrid> result =
        vtkSmartPointer<vtkUnstructuredGrid>::New();
    result->ShallowCopy( converted_grid );

    // vtkCellDataToPointDataはセルに接続していない点へ既定値（通常は0）を
    // 書き込むことがある。maxLevelCellで使われない点や予約点0が正のデータの
    // 最小値を広げないよう、有効セルに属する点のタプルを共有する。
    std::vector<bool> point_has_valid_cell( m_total_point_count, false );
    vtkIdType reference_point = -1;
    for ( vtkIdType cell_id = 0; cell_id < grid->GetNumberOfCells(); ++cell_id )
    {
        vtkCell* cell = grid->GetCell( cell_id );
        if ( !cell || !cell->GetPointIds() || cell->GetNumberOfPoints() == 0 )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename +
                "' produced an invalid cell while repairing unreferenced point data." );
        }

        vtkIdList* point_ids = cell->GetPointIds();
        const vtkIdType first_point = point_ids->GetId( 0 );
        bool degenerate = true;
        for ( vtkIdType local_index = 1;
              local_index < point_ids->GetNumberOfIds(); ++local_index )
        {
            if ( point_ids->GetId( local_index ) != first_point )
            {
                degenerate = false;
                break;
            }
        }
        if ( degenerate )
        {
            continue;
        }

        if ( reference_point < 0 )
        {
            reference_point = first_point;
        }
        for ( vtkIdType local_index = 0;
              local_index < point_ids->GetNumberOfIds(); ++local_index )
        {
            const vtkIdType point_id = point_ids->GetId( local_index );
            if ( point_id < 0 || point_id >= m_vtk_point_count )
            {
                throw std::out_of_range(
                    "MPAS file '" + m_filename +
                    "' produced a cell point id outside the allocated point range." );
            }
            point_has_valid_cell[static_cast<size_t>( point_id )] = true;
        }
    }

    if ( reference_point >= 0 )
    {
        for ( int array_index = 0;
              array_index < result->GetPointData()->GetNumberOfArrays(); ++array_index )
        {
            vtkDataArray* array = result->GetPointData()->GetArray( array_index );
            if ( !array || array->GetNumberOfTuples() != m_vtk_point_count )
            {
                throw std::runtime_error(
                    "MPAS file '" + m_filename +
                    "' produced an invalid point-data array while repairing unreferenced points." );
            }

            std::vector<double> reference_tuple( array->GetNumberOfComponents() );
            array->GetTuple( reference_point, reference_tuple.data() );
            for ( vtkIdType point_id = 0; point_id < m_vtk_point_count; ++point_id )
            {
                if ( !point_has_valid_cell[static_cast<size_t>( point_id )] )
                {
                    array->SetTuple( point_id, reference_tuple.data() );
                }
            }
        }
    }
    result->GetCellData()->Initialize();
    return result;
}

void DirectNetCDFMPASReader::validateResult(
    vtkUnstructuredGrid* result,
    const int cell_type ) const
{
    if ( result->GetNumberOfPoints() == 0 || result->GetNumberOfCells() == 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' produced an empty vtkUnstructuredGrid after cell-to-point conversion." );
    }
    if ( result->GetNumberOfPoints() != m_vtk_point_count ||
         result->GetNumberOfCells() != m_vtk_cell_count )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' changed the expected point or cell count during cell-to-point conversion." );
    }
    for ( vtkIdType cell = 0; cell < result->GetNumberOfCells(); ++cell )
    {
        if ( result->GetCellType( cell ) != cell_type )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename +
                "' has mixed cell types after cell-to-point conversion." );
        }
    }
    if ( result->GetPointData()->GetNumberOfArrays() == 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' has no point data arrays after cell-to-point conversion." );
    }
    for ( int array_index = 0;
          array_index < result->GetPointData()->GetNumberOfArrays(); ++array_index )
    {
        vtkDataArray* array = result->GetPointData()->GetArray( array_index );
        if ( !array ||
             ( array->GetDataType() != VTK_FLOAT && array->GetDataType() != VTK_DOUBLE ) ||
             array->GetNumberOfTuples() != m_vtk_point_count )
        {
            throw std::runtime_error(
                "MPAS file '" + m_filename +
                "' has an invalid point-data tuple count after cell-to-point conversion." );
        }
        for ( vtkIdType tuple = 0; tuple < m_vtk_point_count; ++tuple )
        {
            if ( !std::isfinite( array->GetTuple1( tuple ) ) )
            {
                throw std::runtime_error(
                    "MPAS file '" + m_filename + "' has a non-finite value in point variable '" +
                    std::string( array->GetName() ? array->GetName() : "" ) +
                    "' after missing-value handling." );
            }
        }
    }
    if ( result->GetCellData()->GetNumberOfArrays() != 0 )
    {
        throw std::runtime_error(
            "MPAS file '" + m_filename +
            "' retains cell data after cell-to-point conversion." );
    }
}

vtkSmartPointer<vtkUnstructuredGrid> DirectNetCDFMPASReader::read()
{
    validateInput();
    m_file.open( m_filename );

    readFileMetadata();
    readDimensions();
    readVariableMetadata();
    readCoordinates();
    readConnectivity();
    readMaxLevelCell();
    readSphereAttribute();
    classifyDataVariables();

    vtkSmartPointer<vtkUnstructuredGrid> grid =
        vtkSmartPointer<vtkUnstructuredGrid>::New();
    grid->SetPoints( createPointCoordinates() );

    const int cell_type = m_vertex_degree == 3 ? VTK_WEDGE : VTK_HEXAHEDRON;
    createCells( grid, cell_type );
    addDataArrays( grid );

    vtkSmartPointer<vtkUnstructuredGrid> result = convertCellDataToPointData( grid );
    validateResult( result, cell_type );

    m_file.close();
    return result;
}
} // namespace ExtendedFileFormat
} // namespace kvs
