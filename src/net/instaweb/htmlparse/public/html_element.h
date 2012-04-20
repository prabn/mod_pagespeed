/*
 * Copyright 2010 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: jmarantz@google.com (Joshua Marantz)

#ifndef NET_INSTAWEB_HTMLPARSE_PUBLIC_HTML_ELEMENT_H_
#define NET_INSTAWEB_HTMLPARSE_PUBLIC_HTML_ELEMENT_H_

#include <vector>

#include "net/instaweb/util/public/basictypes.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_parser_types.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class HtmlElement : public HtmlNode {
 public:
  // Tags can be closed in three ways: implicitly (e.g. <img ..>),
  // briefly (e.g. <br/>), or explicitly (<a...>...</a>).  The
  // Lexer will always record the way it parsed a tag, but synthesized
  // elements will have AUTO_CLOSE, and rewritten elements may
  // no longer qualify for the closing style with which they were
  // parsed.
  enum CloseStyle {
    AUTO_CLOSE,      // synthesized tag, or not yet closed in source
    IMPLICIT_CLOSE,  // E.g. <img...> <meta...> <link...> <br...> <input...>
    EXPLICIT_CLOSE,  // E.g. <a href=...>anchor</a>
    BRIEF_CLOSE,     // E.g. <head/>
    UNCLOSED         // Was never closed in source
  };

  class Attribute {
   public:
    // A large quantity of HTML in the wild has attributes that are
    // improperly escaped.  Browsers are generally tolerant of this.
    // But we want to avoid corrupting pages we do not understand.

    // The result of DecodedValueOrNull() and escaped_value() is still
    // owned by this, and will be invalidated by a subsequent call to
    // SetValue() or SetUnescapedValue

    // Returns the attribute name, which is not guaranteed to be case-folded.
    // Compare keyword() to the Keyword constant found in html_name.h for
    // fast attribute comparisons.
    const char* name_str() const { return name_.c_str(); }

    // Returns the HTML keyword enum.  If this attribute name is not
    // recognized, returns HtmlName::kNotAKeyword, and you can examine
    // name_str().
    HtmlName::Keyword keyword() const { return name_.keyword(); }

    HtmlName name() const { return name_; }
    void set_name(const HtmlName& name) { name_ = name; }

    // Returns the value in its original directly from the HTML source.
    // This may have HTML escapes in it, such as "&amp;".
    const char* escaped_value() const { return escaped_value_.get(); }

    // The result of DecodedValueOrNull() is still owned by this, and
    // will be invalidated by a subsequent call to SetValue().
    //
    // The result will be a NUL-terminated string containing the value of the
    // attribute, or NULL if the attribute has no value at all (this is
    // distinct from having the empty string for a value), or there is
    // a decoding error.  E.g.
    //    <tag a="val">              --> "val"
    //    <tag a="&amp;">            --> "&"
    //    <tag a="">                 --> ""
    //    <tag a>                    --> NULL
    //    <tag a="muñecos">          --> NULL  (decoding_error()==true)
    //
    // Returns the unescaped value, suitable for directly operating on
    // in filters as URLs or other data.  Note that decoding_error() is
    // true if the parsed value from HTML could not be decoded.  This
    // might occur if:
    //    - the charset is not known
    //    - the charset is not supported.  Currently none are supported and
    //      only values that fall in 7-bit ascii can be interpreted.
    //    - the charset is known & supported but the value does not appear to be
    //      legal.
    //
    // The decoded value uses 8-bit characters to represent any unicode
    // code-point less than 256.
    const char* DecodedValueOrNull() const {
      return (decoding_error_ ? NULL : value_.get());
    }

    void set_decoding_error(bool x) { decoding_error_ = x; }
    bool decoding_error() const { return decoding_error_; }

    // See comment about quote on constructor for Attribute.
    // Returns the quotation mark associated with this URL, typically
    // ", ', or an empty string.
    const char* quote() const { return quote_; }

    // Two related methods to modify the value of attribute (eg to rewrite
    // dest of src or href). As  with the constructor, copies the string in,
    // so caller retains ownership of value.
    //
    // A StringPiece pointing to an empty string (that is, a char array {'\0'})
    // indicates that the attribute value is the empty string (e.g. <foo
    // bar="">); however, a StringPiece with a data() pointer of NULL indicates
    // that the attribute has no value at all (e.g. <foo bar>).  This is an
    // important distinction.
    //
    // Note that passing a value containing NULs in the middle will cause
    // breakage, but this isn't currently checked for.
    // TODO(mdsteele): Perhaps we should check for this?

    // Sets the value of the attribute.  No HTML escaping is expected.
    // This call causes the HTML-escaped value to be automatically computed
    // by scanning the value and escaping any characters required in HTML
    // attributes.
    void SetValue(const StringPiece& value);

    // Sets the escaped value.  This is intended to be called from the HTML
    // Lexer, and results in the Value being computed automatically by
    // scanning the value for escape sequences.
    void SetEscapedValue(const StringPiece& value);

    // See comment about quote on constructor for Attribute.
    void set_quote(const char *quote) {
      quote_ = quote;
    }

    friend class HtmlElement;

   private:
    // TODO(jmarantz): arg 'quote' must be a static string, or NULL,
    // if quoting is not yet known (e.g. this is a synthesized attribute.
    // This is hard-to-describe and we should probably use an enum for
    // the quote.
    //
    // This should only be called from AddAttribute
    Attribute(const HtmlName& name, const StringPiece& value,
              bool decoding_error, const StringPiece& escaped_value,
              const char* quote);

    static inline void CopyValue(const StringPiece& src,
                                 scoped_array<char>* dst);

    HtmlName name_;
    scoped_array<char> escaped_value_;
    scoped_array<char> value_;
    const char* quote_;
    bool decoding_error_;

    DISALLOW_COPY_AND_ASSIGN(Attribute);
  };

  virtual ~HtmlElement();

  // Add a copy of an attribute to this element.  The attribute may come
  // from this element, or another one.
  void AddAttribute(const Attribute& attr);

  // Unconditionally add attribute, copying value.
  // For binary attributes (those without values) use value=NULL.
  // TODO(sligocki): StringPiece(NULL) seems fragile because what it is or
  // how it's treated is not docutmented.
  //
  // quote is assumed to be a static const char *.
  // Doesn't check for attribute duplication (which is illegal in html).
  //
  // The value, if non-null, is assumed to be unescaped.  See also
  // AddEscapedAttribute.
  void AddAttribute(const HtmlName& name,
                    const StringPiece& value,
                    const char* quote);
  // As AddAttribute, but assumes value has been escaped for html output.
  void AddEscapedAttribute(const HtmlName& name,
                           const StringPiece& escaped_value,
                           const char* quote);

  // Removes the attribute at the given index, shifting higher-indexed
  // attributes down.  Note that this operation is linear in the number of
  // attributes.
  void DeleteAttribute(int i);

  // Remove the attribute with the given name.  Return true if the attribute
  // was deleted, false if it wasn't there to begin with.
  bool DeleteAttribute(HtmlName::Keyword keyword);

  // Look up attribute by name.  NULL if no attribute exists.
  // Use this for attributes whose value you might want to change
  // after lookup.
  const Attribute* FindAttribute(HtmlName::Keyword keyword) const;
  Attribute* FindAttribute(HtmlName::Keyword keyword) {
    const HtmlElement* const_this = this;
    const Attribute* result = const_this->FindAttribute(keyword);
    return const_cast<Attribute*>(result);
  }

  // Look up attribute value by name.
  // Returns NULL if:
  //    1. no attribute exists
  //    2. the attribute has no value.
  //    3. the attribute has a value, but it cannot currently be safely decoded.
  // If you care about this distinction, call FindAttribute.
  // Use this only if you don't intend to change the attribute value;
  // if you might change the attribute value, use FindAttribute instead
  // (this avoids a double lookup).
  const char* AttributeValue(HtmlName::Keyword name) const {
    const Attribute* attribute = FindAttribute(name);
    if (attribute != NULL) {
      return attribute->DecodedValueOrNull();
    }
    return NULL;
  }

  // Small integer uniquely identifying the HTML element, primarily
  // for debugging.
  void set_sequence(int sequence) { sequence_ = sequence; }

  // Returns the element tag name, which is not guaranteed to be
  // case-folded.  Compare keyword() to the Keyword constant found in
  // html_name.h for fast tag name comparisons.
  const char* name_str() const { return name_.c_str(); }

  // Returns the HTML keyword enum.  If this tag name is not
  // recognized, returns HtmlName::kNotAKeyword, and you can
  // examine name_str().
  HtmlName::Keyword keyword() const { return name_.keyword(); }

  const HtmlName& name() const { return name_; }

  // Changing that tag of an element should only occur if the caller knows
  // that the old attributes make sense for the new tag.  E.g. a div could
  // be changed to a span.
  void set_name(const HtmlName& new_tag) { name_ = new_tag; }

  int attribute_size() const {return attributes_.size(); }
  const Attribute& attribute(int i) const { return *attributes_[i]; }
  Attribute& attribute(int i) { return *attributes_[i]; }

  friend class HtmlParse;
  friend class HtmlLexer;

  CloseStyle close_style() const { return close_style_; }
  void set_close_style(CloseStyle style) { close_style_ = style; }

  // Render an element as a string for debugging.  This is not
  // intended as a fully legal serialization.
  void ToString(GoogleString* buf) const;
  void DebugPrint() const;

  int begin_line_number() const { return begin_line_number_; }
  int end_line_number() const { return end_line_number_; }

 protected:
  virtual void SynthesizeEvents(const HtmlEventListIterator& iter,
                                HtmlEventList* queue);
  virtual void InvalidateIterators(const HtmlEventListIterator& end);

  virtual HtmlEventListIterator begin() const { return begin_; }
  virtual HtmlEventListIterator end() const { return end_; }

 private:
  // Begin/end event iterators are used by HtmlParse to keep track
  // of the span of events underneath an element.  This is primarily to
  // help delete the element.  Events are not public.
  void set_begin(const HtmlEventListIterator& begin) { begin_ = begin; }
  void set_end(const HtmlEventListIterator& end) { end_ = end; }

  void set_begin_line_number(int line) { begin_line_number_ = line; }
  void set_end_line_number(int line) { end_line_number_ = line; }

  // construct via HtmlParse::NewElement
  HtmlElement(HtmlElement* parent, const HtmlName& name,
              const HtmlEventListIterator& begin,
              const HtmlEventListIterator& end);

  int sequence_;
  HtmlName name_;
  std::vector<Attribute*> attributes_;
  HtmlEventListIterator begin_;
  HtmlEventListIterator end_;
  CloseStyle close_style_;
  int begin_line_number_;
  int end_line_number_;

  DISALLOW_COPY_AND_ASSIGN(HtmlElement);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTMLPARSE_PUBLIC_HTML_ELEMENT_H_
