// SPDX-License-Identifier: GPL-3.0-or-later
//
// A minimal, non-validating XML reader for OpenRaster stack.xml: elements, attributes (qualified
// names kept verbatim, e.g. "rl:id"), the five predefined entities and numeric character
// references. Text content, comments, processing instructions, DOCTYPE and CDATA are skipped.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rl::io {

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlElement> children;

    const std::string* attr(const std::string& k) const {
        for (const auto& a : attrs)
            if (a.first == k) return &a.second;
        return nullptr;
    }
};

// Returns the document element. Throws IoError on malformed input.
XmlElement parse_xml(const std::string& text);

// Escapes &, <, >, " and ' for use in an attribute value.
std::string xml_escape(const std::string& s);

}  // namespace rl::io
