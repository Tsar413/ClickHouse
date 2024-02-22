#include <Columns/ColumnConst.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsNumber.h>
#include <Common/BitHelpers.h>
#include <Functions/FunctionSpaceFillingCurveEncode.h>
#include <Functions/IFunction.h>
#include <Functions/PerformanceAdaptors.h>
#include <limits>


namespace DB
{

namespace HilbertDetails
{

template <UInt8 bit_step>
class HilbertLookupTable {
public:
    constexpr static UInt8 LOOKUP_TABLE[0] = {};
};

template <>
class HilbertLookupTable<1> {
public:
    constexpr static UInt8 LOOKUP_TABLE[16] = {
        4, 1, 11, 2,
        0, 15, 5, 6,
        10, 9, 3, 12,
        14, 7, 13, 8
    };
};

template <>
class HilbertLookupTable<3> {
public:
    constexpr static UInt8 LOOKUP_TABLE[256] = {
        64, 1, 206, 79, 16, 211, 84, 21, 131, 2, 205, 140, 81, 82, 151, 22, 4, 199, 8, 203, 158,
        157, 88, 25, 69, 70, 73, 74, 31, 220, 155, 26, 186, 185, 182, 181, 32, 227, 100, 37, 59,
        248, 55, 244, 97, 98, 167, 38, 124, 61, 242, 115, 174, 173, 104, 41, 191, 62, 241, 176, 47,
        236, 171, 42, 0, 195, 68, 5, 250, 123, 60, 255, 65, 66, 135, 6, 249, 184, 125, 126, 142,
        141, 72, 9, 246, 119, 178, 177, 15, 204, 139, 10, 245, 180, 51, 240, 80, 17, 222, 95, 96,
        33, 238, 111, 147, 18, 221, 156, 163, 34, 237, 172, 20, 215, 24, 219, 36, 231, 40, 235, 85,
        86, 89, 90, 101, 102, 105, 106, 170, 169, 166, 165, 154, 153, 150, 149, 43, 232, 39, 228,
        27, 216, 23, 212, 108, 45, 226, 99, 92, 29, 210, 83, 175, 46, 225, 160, 159, 30, 209, 144,
        48, 243, 116, 53, 202, 75, 12, 207, 113, 114, 183, 54, 201, 136, 77, 78, 190, 189, 120, 57,
        198, 71, 130, 129, 63, 252, 187, 58, 197, 132, 3, 192, 234, 107, 44, 239, 112, 49, 254,
        127, 233, 168, 109, 110, 179, 50, 253, 188, 230, 103, 162, 161, 52, 247, 56, 251, 229, 164,
        35, 224, 117, 118, 121, 122, 218, 91, 28, 223, 138, 137, 134, 133, 217, 152, 93, 94, 11,
        200, 7, 196, 214, 87, 146, 145, 76, 13, 194, 67, 213, 148, 19, 208, 143, 14, 193, 128,
    };
};

}


template <UInt8 bit_step = 3>
class FunctionHilbertEncode2DWIthLookupTableImpl
{
public:
    struct HilbertEncodeState {
        UInt64 hilbert_code = 0;
        UInt8 state = 0;
    };

    static UInt64 encode(UInt64 x, UInt64 y)
    {
        return encodeFromState(x, y, 0).hilbert_code;
    }

    static HilbertEncodeState encodeFromState(UInt64 x, UInt64 y, UInt8 state)
    {
        HilbertEncodeState result;
        result.state = state;
        const auto leading_zeros_count = getLeadingZeroBits(x | y);
        const auto used_bits = std::numeric_limits<UInt64>::digits - leading_zeros_count;

        auto [iterations, current_shift] = getIterationsAndInitialShift(used_bits);

        for (; iterations > 0; --iterations, current_shift -= bit_step)
        {
            if (iterations % 2 == 0) {
                std::swap(x, y);
            }
            const UInt8 x_bits = (x >> current_shift) & STEP_MASK;
            const UInt8 y_bits = (y >> current_shift) & STEP_MASK;
            const auto current_step_state = getCodeAndUpdateState(x_bits, y_bits, result.state);
            result.hilbert_code |= (current_step_state.hilbert_code << getHilbertShift(current_shift));
            result.state = current_step_state.state;
        }

        return result;
    }

private:
    // LOOKUP_TABLE[SSXXXYYY] = SSHHHHHH
    // where SS - 2 bits for state, XXX - 3 bits of x, YYY - 3 bits of y
    // State is rotation of curve on every step, left/up/right/down - therefore 2 bits
    static HilbertEncodeState getCodeAndUpdateState(UInt8 x_bits, UInt8 y_bits, UInt8 state)
    {
        HilbertEncodeState result;
        const UInt8 table_index = state | (x_bits << bit_step) | y_bits;
        const auto table_code = HilbertDetails::HilbertLookupTable<bit_step>::LOOKUP_TABLE[table_index];
        result.state = table_code & STATE_MASK;
        result.hilbert_code = table_code & HILBERT_MASK;
        return result;
    }

    // hilbert code is double size of input values
    static constexpr UInt8 getHilbertShift(UInt8 shift)
    {
        return shift << 1;
    }

    static std::pair<UInt8, UInt8> getIterationsAndInitialShift(UInt8 used_bits)
    {
        UInt8 iterations = used_bits / bit_step;
        UInt8 initial_shift = iterations * bit_step;
        if (initial_shift < used_bits)
        {
            ++iterations;
        } else {
            initial_shift -= bit_step;
        }
        return {iterations, initial_shift};
    }

    constexpr static UInt8 STEP_MASK = (1 << bit_step) - 1;
    constexpr static UInt8 HILBERT_MASK = (1 << getHilbertShift(bit_step)) - 1;
    constexpr static UInt8 STATE_MASK = 0b11 << getHilbertShift(bit_step);
};


class FunctionHilbertEncode : public FunctionSpaceFillingCurveEncode
{
public:
    static constexpr auto name = "hilbertEncode";
    static FunctionPtr create(ContextPtr)
    {
        return std::make_shared<FunctionHilbertEncode>();
    }

    String getName() const override { return name; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        size_t num_dimensions = arguments.size();
        if (num_dimensions < 1 || num_dimensions > 2) {
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal number of UInt arguments of function {}: should be at least 1 and not more than 2",
                getName());
        }

        size_t vector_start_index = 0;
        const auto * const_col = typeid_cast<const ColumnConst *>(arguments[0].column.get());
        const ColumnTuple * mask;
        if (const_col)
            mask = typeid_cast<const ColumnTuple *>(const_col->getDataColumnPtr().get());
        else
            mask = typeid_cast<const ColumnTuple *>(arguments[0].column.get());
        if (mask)
        {
            num_dimensions = mask->tupleSize();
            vector_start_index = 1;
            for (size_t i = 0; i < num_dimensions; i++)
            {
                auto ratio = mask->getColumn(i).getUInt(0);
                if (ratio > 8 || ratio < 1)
                    throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND,
                        "Illegal argument {} of function {}, should be a number in range 1-8",
                        arguments[0].column->getName(), getName());
            }
        }

        auto non_const_arguments = arguments;
        for (auto & argument : non_const_arguments)
            argument.column = argument.column->convertToFullColumnIfConst();

        auto col_res = ColumnUInt64::create();
        ColumnUInt64::Container & vec_res = col_res->getData();
        vec_res.resize(input_rows_count);

        const ColumnPtr & col0 = non_const_arguments[0 + vector_start_index].column;
        if (num_dimensions == 1)
        {
            for (size_t i = 0; i < input_rows_count; ++i)
            {
                vec_res[i] = col0->getUInt(i);
            }
            return col_res;
        }

        const ColumnPtr & col1 = non_const_arguments[1 + vector_start_index].column;
        for (size_t i = 0; i < input_rows_count; ++i)
        {
            vec_res[i] = FunctionHilbertEncode2DWIthLookupTableImpl<3>::encode(col0->getUInt(i), col1->getUInt(i));
        }
        return col_res;
    }
};

}
