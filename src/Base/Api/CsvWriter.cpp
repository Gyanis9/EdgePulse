#include "Base/Api/CsvWriter.h"

namespace EdgePulse::Common
{
    void CsvWriter::addRow(const std::vector<std::string> &cells)
    {
        for (std::size_t index = 0; index < cells.size(); ++index)
        {
            if (index != 0)
            {
                m_text.push_back(',');
            }
            m_text += escape(cells[index]);
        }
        m_text += "\r\n";
    }

    std::string CsvWriter::take() const
    {
        return "\xEF\xBB\xBF" + m_text;
    }

    std::string CsvWriter::escape(const std::string_view value)
    {
        const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string_view::npos;
        if (!needsQuotes)
        {
            return std::string{value};
        }
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (const char character: value)
        {
            if (character == '"')
            {
                escaped.push_back('"');
            }
            escaped.push_back(character);
        }
        escaped.push_back('"');
        return escaped;
    }
} // namespace EdgePulse::Common
