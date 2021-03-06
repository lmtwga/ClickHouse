#include <DataStreams/SummingSortedBlockInputStream.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNested.h>
#include <DataTypes/DataTypeArray.h>
#include <Columns/ColumnTuple.h>
#include <Common/StringUtils.h>
#include <Core/FieldVisitors.h>
#include <common/logger_useful.h>
#include <Common/typeid_cast.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <Functions/FunctionFactory.h>
#include <Interpreters/Context.h>

namespace DB
{


String SummingSortedBlockInputStream::getID() const
{
    std::stringstream res;
    res << "SummingSorted(inputs";

    for (size_t i = 0; i < children.size(); ++i)
        res << ", " << children[i]->getID();

    res << ", description";

    for (size_t i = 0; i < description.size(); ++i)
        res << ", " << description[i].getID();

    res << ")";
    return res.str();
}


void SummingSortedBlockInputStream::insertCurrentRow(ColumnPlainPtrs & merged_columns)
{
    for (auto & desc : columns_to_aggregate)
    {
        // Do not insert if the aggregation state hasn't been created
        if (desc.created)
        {
            try
            {
                desc.function->insertResultInto(desc.state.data(), *desc.merged_column);
            }
            catch (...)
            {
                desc.function->destroy(desc.state.data());
                desc.created = false;
                throw;
            }
            desc.function->destroy(desc.state.data());
            desc.created = false;
        }
        else
            desc.merged_column->insertDefault();
    }

    for (auto i : column_numbers_not_to_aggregate)
        merged_columns[i]->insert(current_row[i]);
}


namespace
{
    bool isInPrimaryKey(const SortDescription & description, const std::string & name, const size_t number)
    {
        for (auto & desc : description)
            if (desc.column_name == name || (desc.column_name.empty() && desc.column_number == number))
                return true;

        return false;
    }
}


Block SummingSortedBlockInputStream::readImpl()
{
    if (finished)
        return Block();

    if (children.size() == 1)
        return children[0]->read();

    Block merged_block;
    ColumnPlainPtrs merged_columns;

    init(merged_block, merged_columns);
    if (merged_columns.empty())
        return Block();

    /// Additional initialization.
    if (current_row.empty())
    {
        auto & factory = AggregateFunctionFactory::instance();

        current_row.resize(num_columns);
        next_key.columns.resize(description.size());

        /// name of nested structure -> the column numbers that refer to it.
        std::unordered_map<std::string, std::vector<size_t>> discovered_maps;

        /** Fill in the column numbers, which must be summed.
          * This can only be numeric columns that are not part of the sort key.
          * If a non-empty column_names_to_sum is specified, then we only take these columns.
          * Some columns from column_names_to_sum may not be found. This is ignored.
          */
        for (size_t i = 0; i < num_columns; ++i)
        {
            ColumnWithTypeAndName & column = merged_block.safeGetByPosition(i);

            /// Discover nested Maps and find columns for summation
            if (typeid_cast<const DataTypeArray *>(column.type.get()))
            {
                const auto map_name = DataTypeNested::extractNestedTableName(column.name);
                /// if nested table name ends with `Map` it is a possible candidate for special handling
                if (map_name == column.name || !endsWith(map_name, "Map"))
                {
                    column_numbers_not_to_aggregate.push_back(i);
                    continue;
                }

                discovered_maps[map_name].emplace_back(i);
            }
            else
            {
                /// Leave only numeric types. Note that dates and datetime here are not considered such.
                if (!column.type->isNumeric() ||
                    column.type->getName() == "Date" ||
                    column.type->getName() == "DateTime" ||
                    column.type->getName() == "Nullable(Date)" ||
                    column.type->getName() == "Nullable(DateTime)")
                {
                    column_numbers_not_to_aggregate.push_back(i);
                    continue;
                }

                /// Are they inside the PK?
                if (isInPrimaryKey(description, column.name, i))
                {
                    column_numbers_not_to_aggregate.push_back(i);
                    continue;
                }

                if (column_names_to_sum.empty()
                    || column_names_to_sum.end() !=
                       std::find(column_names_to_sum.begin(), column_names_to_sum.end(), column.name))
                {
                    // Create aggregator to sum this column
                    auto desc = AggregateDescription{};
                    desc.column_numbers = {i};
                    desc.merged_column = column.column;
                    desc.function = factory.get("sum", {column.type});
                    desc.function->setArguments({column.type});
                    desc.state.resize(desc.function->sizeOfData());
                    columns_to_aggregate.emplace_back(std::move(desc));
                }
            }
        }

        /// select actual nested Maps from list of candidates
        for (const auto & map : discovered_maps)
        {
            /// map should contain at least two elements (key -> value)
            if (map.second.size() < 2)
                continue;

            /// no elements of map could be in primary key
            auto column_num_it = map.second.begin();
            for (; column_num_it != map.second.end(); ++column_num_it)
                if (isInPrimaryKey(description, merged_block.safeGetByPosition(*column_num_it).name, *column_num_it))
                    break;
            if (column_num_it != map.second.end())
                continue;

            // Wrap aggregated columns in a tuple to match function signature
            DataTypes argument_types = {};
            auto tuple = std::make_shared<ColumnTuple>();
            auto & tuple_columns = tuple->getColumns();
            auto desc = AggregateDescription{};
            auto map_desc = MapDescription{};

            column_num_it = map.second.begin();
            for (; column_num_it != map.second.end(); ++column_num_it)
            {
                const ColumnWithTypeAndName & key_col = merged_block.safeGetByPosition(*column_num_it);
                const String & name = key_col.name;
                const IDataType & nested_type = *static_cast<const DataTypeArray *>(key_col.type.get())->getNestedType();

                if (column_num_it == map.second.begin()
                    || endsWith(name, "ID")
                    || endsWith(name, "Key")
                    || endsWith(name, "Type"))
                {
                    if (!nested_type.isNumeric()
                        || nested_type.getName() == "Float32"
                        || nested_type.getName() == "Float64")
                        break;

                    map_desc.key_col_nums.push_back(*column_num_it);
                }
                else
                {
                    if (!nested_type.behavesAsNumber())
                        break;

                    map_desc.val_col_nums.push_back(*column_num_it);
                }

                // Add column to function arguments
                desc.column_numbers.push_back(*column_num_it);
                argument_types.push_back(key_col.type);
                tuple_columns.push_back(key_col.column);
            }

            if (column_num_it != map.second.end())
                continue;

            if (map_desc.key_col_nums.size() == 1)
            {
                // Create summation for all value columns in the map
                desc.merged_column = static_cast<ColumnPtr>(tuple);
                desc.function = factory.get("sumMap", argument_types);
                desc.function->setArguments(argument_types);
                desc.state.resize(desc.function->sizeOfData());
                columns_to_aggregate.emplace_back(std::move(desc));
            }
            else
            {
                // Fall back to legacy mergeMaps for composite keys
                for (auto i : map_desc.key_col_nums)
                    column_numbers_not_to_aggregate.push_back(i);
                for (auto i : map_desc.val_col_nums)
                    column_numbers_not_to_aggregate.push_back(i);
                maps_to_sum.emplace_back(std::move(map_desc));
            }
        }
    }

    if (has_collation)
        merge(merged_columns, queue_with_collation);
    else
        merge(merged_columns, queue);

    return merged_block;
}


template <typename TSortCursor>
void SummingSortedBlockInputStream::merge(ColumnPlainPtrs & merged_columns, std::priority_queue<TSortCursor> & queue)
{
    size_t merged_rows = 0;

    /// Take the rows in needed order and put them in `merged_block` until rows no more than `max_block_size`
    while (!queue.empty())
    {
        TSortCursor current = queue.top();

        setPrimaryKeyRef(next_key, current);

        bool key_differs;

        if (current_key.empty())    /// The first key encountered.
         {
            current_key.columns.resize(description.size());
            setPrimaryKeyRef(current_key, current);
            key_differs = true;
        }
        else
            key_differs = next_key != current_key;

        /// if there are enough rows and the last one is calculated completely
        if (key_differs && merged_rows >= max_block_size)
            return;

        queue.pop();

        if (key_differs)
        {
            /// Write the data for the previous group.
            if (!current_row_is_zero)
            {
                ++merged_rows;
                output_is_non_empty = true;
                insertCurrentRow(merged_columns);
            }

            current_key.swap(next_key);

            setRow(current_row, current);
            current_row_is_zero = false;

            /// Reset aggregation states for next row
            for (auto & desc : columns_to_aggregate)
            {
                desc.function->create(desc.state.data());
                desc.created = true;
            }
        }
        else
        {
            // Merge maps only for same rows
            for (auto & desc : maps_to_sum)
            {
                if (mergeMap(desc, current_row, current))
                    current_row_is_zero = false;
            }
        }

        if (addRow(current_row, current))
            current_row_is_zero = false;

        if (!current->isLast())
        {
            current->next();
            queue.push(current);
        }
        else
        {
            /// We get the next block from the corresponding source, if there is one.
            fetchNextBlock(current, queue);
        }
    }

    /// We will write the data for the last group, if it is non-zero.
    /// If it is zero, and without it the output stream will be empty, we will write it anyway.
    if (!current_row_is_zero || !output_is_non_empty)
    {
        ++merged_rows;  /// Dead store (result is unused). Left for clarity.
        insertCurrentRow(merged_columns);
    }

    finished = true;
}

template <typename TSortCursor>
bool SummingSortedBlockInputStream::mergeMap(const MapDescription & desc, Row & row, TSortCursor & cursor)
{
    /// Strongly non-optimal.

    Row & left = row;
    Row right(left.size());

    for (size_t col_num : desc.key_col_nums)
        right[col_num] = (*cursor->all_columns[col_num])[cursor->pos].template get<Array>();

    for (size_t col_num : desc.val_col_nums)
        right[col_num] = (*cursor->all_columns[col_num])[cursor->pos].template get<Array>();

    auto at_ith_column_jth_row = [&](const Row & matrix, size_t i, size_t j) -> const Field &
    {
        return matrix[i].get<Array>()[j];
    };

    auto tuple_of_nth_columns_at_jth_row = [&](const Row & matrix, const ColumnNumbers & col_nums, size_t j) -> Array
    {
        size_t size = col_nums.size();
        Array res(size);
        for (size_t col_num_index = 0; col_num_index < size; ++col_num_index)
            res[col_num_index] = at_ith_column_jth_row(matrix, col_nums[col_num_index], j);
        return res;
    };

    std::map<Array, Array> merged;

    auto accumulate = [](Array & dst, const Array & src)
    {
        bool has_non_zero = false;
        size_t size = dst.size();
        for (size_t i = 0; i < size; ++i)
            if (applyVisitor(FieldVisitorSum(src[i]), dst[i]))
                has_non_zero = true;
        return has_non_zero;
    };

    auto merge = [&](const Row & matrix)
    {
        size_t rows = matrix[desc.key_col_nums[0]].get<Array>().size();

        for (size_t j = 0; j < rows; ++j)
        {
            Array key = tuple_of_nth_columns_at_jth_row(matrix, desc.key_col_nums, j);
            Array value = tuple_of_nth_columns_at_jth_row(matrix, desc.val_col_nums, j);

            auto it = merged.find(key);
            if (merged.end() == it)
                merged.emplace(std::move(key), std::move(value));
            else
            {
                if (!accumulate(it->second, value))
                    merged.erase(it);
            }
        }
    };

    merge(left);
    merge(right);

    for (size_t col_num : desc.key_col_nums)
        row[col_num] = Array(merged.size());
    for (size_t col_num : desc.val_col_nums)
        row[col_num] = Array(merged.size());

    size_t row_num = 0;
    for (const auto & key_value : merged)
    {
        for (size_t col_num_index = 0, size = desc.key_col_nums.size(); col_num_index < size; ++col_num_index)
            row[desc.key_col_nums[col_num_index]].get<Array>()[row_num] = key_value.first[col_num_index];

        for (size_t col_num_index = 0, size = desc.val_col_nums.size(); col_num_index < size; ++col_num_index)
            row[desc.val_col_nums[col_num_index]].get<Array>()[row_num] = key_value.second[col_num_index];

        ++row_num;
    }

    return row_num != 0;
}


template <typename TSortCursor>
bool SummingSortedBlockInputStream::addRow(Row & row, TSortCursor & cursor)
{
    bool res = false;
    for (auto & desc : columns_to_aggregate)
    {
        if (desc.created)
        {
            // Specialized case for unary functions
            if (desc.column_numbers.size() == 1)
            {
                auto & col = cursor->all_columns[desc.column_numbers[0]];
                desc.function->add(desc.state.data(), &col, cursor->pos, nullptr);
                // Flag row as non-empty if at least one column number if non-zero
                // Note: This defers compaction of signed type rows that sum to zero by one merge
                if (!res)
                    res = col->get64(cursor->pos) != 0;
            }
            else
            {
                // Gather all source columns into a vector
                ConstColumnPlainPtrs columns(desc.column_numbers.size());
                for (size_t i = 0; i < desc.column_numbers.size(); ++i)
                    columns[i] = cursor->all_columns[desc.column_numbers[i]];

                desc.function->add(desc.state.data(), columns.data(), cursor->pos, nullptr);
            }
        }
    }

    return res;
}

}
