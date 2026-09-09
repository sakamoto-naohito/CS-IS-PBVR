#ifndef EXTENDED_FILE_FORMAT__NUMERAL_SEQUENCE_FILE_NAMES_H_INCLUDE
#define EXTENDED_FILE_FORMAT__NUMERAL_SEQUENCE_FILE_NAMES_H_INCLUDE

#include <string>
#include <vector>

#include <vtkGlobFileNames.h>
#include <vtkNew.h>
#include <vtkSmartPointer.h>
#include <vtkSortFileNames.h>
#include <vtkStringArray.h>

namespace kvs
{
namespace ExtendedFileFormat
{

class NumeralSequenceFileNames
{
public:
    NumeralSequenceFileNames( const std::string& pattern )
    {
        vtkNew<vtkGlobFileNames> glob;
        glob->RecurseOff();
        glob->AddFileNames( pattern.c_str() );

        vtkStringArray* matched_files = glob->GetFileNames();

        vtkNew<vtkSortFileNames> sorter;
        sorter->GroupingOff();
        sorter->NumericSortOn();
        sorter->IgnoreCaseOff();
        sorter->SkipDirectoriesOn();
        sorter->SetInputFileNames( matched_files );

        filenames = sorter->GetFileNames();
    }

public:
    int numberOfFiles() const
    {
        return filenames->GetNumberOfValues();
    }

    std::vector<std::string> fileNames() const
    {
        std::vector<std::string> result;

        const vtkIdType number_of_files =
            filenames->GetNumberOfValues();

        result.reserve( number_of_files );

        for ( vtkIdType i = 0; i < number_of_files; ++i )
        {
            result.emplace_back(
                filenames->GetValue( i )
            );
        }

        return result;
    }

private:
    vtkSmartPointer<vtkStringArray> filenames;
};

} // namespace ExtendedFileFormat
} // namespace kvs

#endif // EXTENDED_FILE_FORMAT__NUMERAL_SEQUENCE_FILE_NAMES_H_INCLUDE
