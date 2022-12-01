// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "google/protobuf/compiler/objectivec/import_writer.h"

#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "google/protobuf/compiler/objectivec/line_consumer.h"
#include "google/protobuf/compiler/objectivec/names.h"
#include "google/protobuf/io/printer.h"

// NOTE: src/google/protobuf/compiler/plugin.cc makes use of cerr for some
// error cases, so it seems to be ok to use as a back door for errors.

namespace google {
namespace protobuf {
namespace compiler {
namespace objectivec {

namespace {

class ProtoFrameworkCollector : public LineConsumer {
 public:
  explicit ProtoFrameworkCollector(
      absl::flat_hash_map<std::string, std::string>*
          inout_proto_file_to_framework_name)
      : map_(inout_proto_file_to_framework_name) {}

  bool ConsumeLine(absl::string_view line, std::string* out_error) override;

 private:
  absl::flat_hash_map<std::string, std::string>* map_;
};

bool ProtoFrameworkCollector::ConsumeLine(absl::string_view line,
                                          std::string* out_error) {
  int offset = line.find(':');
  if (offset == absl::string_view::npos) {
    *out_error = absl::StrCat(
        "Framework/proto file mapping line without colon sign: '", line, "'.");
    return false;
  }
  absl::string_view framework_name =
      absl::StripAsciiWhitespace(line.substr(0, offset));
  absl::string_view proto_file_list =
      absl::StripAsciiWhitespace(line.substr(offset + 1));

  int start = 0;
  while (start < proto_file_list.length()) {
    offset = proto_file_list.find(',', start);
    if (offset == absl::string_view::npos) {
      offset = proto_file_list.length();
    }

    absl::string_view proto_file = absl::StripAsciiWhitespace(
        proto_file_list.substr(start, offset - start));
    if (!proto_file.empty()) {
      auto existing_entry = map_->find(proto_file);
      if (existing_entry != map_->end()) {
        std::cerr << "warning: duplicate proto file reference, replacing "
                     "framework entry for '"
                  << proto_file << "' with '" << framework_name << "' (was '"
                  << existing_entry->second << "')." << std::endl;
        std::cerr.flush();
      }

      if (absl::StrContains(proto_file, ' ')) {
        std::cerr << "note: framework mapping file had a proto file with a "
                     "space in, hopefully that isn't a missing comma: '"
                  << proto_file << "'" << std::endl;
        std::cerr.flush();
      }

      (*map_)[proto_file] = std::string(framework_name);
    }

    start = offset + 1;
  }

  return true;
}

}  // namespace

ImportWriter::ImportWriter(
    const std::string& generate_for_named_framework,
    const std::string& named_framework_to_proto_path_mappings_path,
    const std::string& runtime_import_prefix, bool for_bundled_proto)
    : generate_for_named_framework_(generate_for_named_framework),
      named_framework_to_proto_path_mappings_path_(
          named_framework_to_proto_path_mappings_path),
      runtime_import_prefix_(runtime_import_prefix),
      for_bundled_proto_(for_bundled_proto),
      need_to_parse_mapping_file_(true) {}

void ImportWriter::AddFile(const FileDescriptor* file,
                           const std::string& header_extension) {
  if (IsProtobufLibraryBundledProtoFile(file)) {
    // The imports of the WKTs are only needed within the library itself,
    // in other cases, they get skipped because the generated code already
    // import GPBProtocolBuffers.h and hence proves them.
    if (for_bundled_proto_) {
      const std::string header_name =
          "GPB" + FilePathBasename(file) + header_extension;
      protobuf_imports_.push_back(header_name);
    }
    return;
  }

  // Lazy parse any mappings.
  if (need_to_parse_mapping_file_) {
    ParseFrameworkMappings();
  }

  auto proto_lookup = proto_file_to_framework_name_.find(file->name());
  if (proto_lookup != proto_file_to_framework_name_.end()) {
    other_framework_imports_.push_back(
        proto_lookup->second + "/" + FilePathBasename(file) + header_extension);
    return;
  }

  if (!generate_for_named_framework_.empty()) {
    other_framework_imports_.push_back(generate_for_named_framework_ + "/" +
                                       FilePathBasename(file) +
                                       header_extension);
    return;
  }

  other_imports_.push_back(FilePath(file) + header_extension);
}

void ImportWriter::AddRuntimeImport(const std::string& header_name) {
  protobuf_imports_.push_back(header_name);
}

void ImportWriter::EmitFileImports(io::Printer* p) const {
  p->Emit(
      {
          {"other_framework_imports",
           [&] {
             for (const auto& header : other_framework_imports_) {
               p->Emit({{"header", header}},
                       R"objc(
                         #import <$header$>
                       )objc");
             }
           }},
          {"other_imports",
           [&] {
             for (const auto& header : other_imports_) {
               p->Emit({{"header", header}},
                       R"objc(
                         #import "$header$"
                       )objc");
             }
           }},
      },
      R"objc(
        $other_framework_imports$;
        $other_imports$;
      )objc");
}

void ImportWriter::EmitRuntimeImports(io::Printer* p,
                                      bool default_cpp_symbol) const {
  EmitRuntimeImports(p, protobuf_imports_, runtime_import_prefix_,
                     for_bundled_proto_, default_cpp_symbol);
}

void ImportWriter::EmitRuntimeImports(
    io::Printer* p, const std::vector<std::string>& headers_to_import,
    const std::string& runtime_import_prefix, bool is_bundled_proto,
    bool default_cpp_symbol) {
  // Given an override, use that.
  if (!runtime_import_prefix.empty()) {
    p->Emit(
        {
            {"import_prefix", runtime_import_prefix},
            {"imports",
             [&] {
               for (const auto& header : headers_to_import) {
                 p->Emit({{"header", header}},
                         R"objc(
                           #import "$import_prefix$/$header$"
                         )objc");
               }
             }},
        },
        R"objc(
          $imports$;
        )objc");

    return;
  }

  // If bundled, no need to do the framework support below.
  if (is_bundled_proto) {
    GOOGLE_CHECK(!default_cpp_symbol);
    p->Emit(
        {
            {"imports",
             [&] {
               for (const auto& header : headers_to_import) {
                 p->Emit({{"header", header}},
                         R"objc(
                           #import "$header$"
                         )objc");
               }
             }},
        },
        R"objc(
          $imports$;
        )objc");
    return;
  }

  auto v = p->WithVars({
      {"cpp_symbol",
       ProtobufFrameworkImportSymbol(ProtobufLibraryFrameworkName)},
  });

  if (default_cpp_symbol) {
    p->Emit(
        R"objc(
          // This CPP symbol can be defined to use imports that match up to the framework
          // imports needed when using CocoaPods.
          #if !defined($cpp_symbol$)
           #define $cpp_symbol$ 0
          #endif

        )objc");
  }

  p->Emit(
      {
          {"framework_name", ProtobufLibraryFrameworkName},
          {"framework_imports",
           [&] {
             for (const auto& header : headers_to_import) {
               p->Emit({{"header", header}},
                       R"objc(
                         #import <$framework_name$/$header$>
                       )objc");
             }
           }},
          {"raw_imports",
           [&] {
             for (const auto& header : headers_to_import) {
               p->Emit({{"header", header}},
                       R"objc(
                         #import "$header$"
                       )objc");
             }
           }},
      },
      R"objc(
        #if $cpp_symbol$
         $framework_imports$
        #else
         $raw_imports$
        #endif
      )objc");
}

void ImportWriter::ParseFrameworkMappings() {
  need_to_parse_mapping_file_ = false;
  if (named_framework_to_proto_path_mappings_path_.empty()) {
    return;  // Nothing to do.
  }

  ProtoFrameworkCollector collector(&proto_file_to_framework_name_);
  std::string parse_error;
  if (!ParseSimpleFile(named_framework_to_proto_path_mappings_path_, &collector,
                       &parse_error)) {
    std::cerr << "error parsing "
              << named_framework_to_proto_path_mappings_path_ << " : "
              << parse_error << std::endl;
    std::cerr.flush();
  }
}

}  // namespace objectivec
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
