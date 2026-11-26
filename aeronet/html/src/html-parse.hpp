#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct lexbor_hash;
struct lxb_dom_node;
struct lxb_html_document;

namespace aeronet::internal {

// The tree construction of HTML walks the stack of open elements for most start tags (the "has an element in scope"
// checks), and recreates the list of active formatting elements that are no longer open before most text and start
// tags (misnested <b>, <font>...): unbounded, both make the parsing quadratic, in time for deeply nested elements, in
// time and nodes for a list that keeps growing (150 KB of "<p><b id=N>x</p>" created 32 million elements).
// The text nodes that the tree builder appends characters to are also given room to grow, as their memory was quadratic
// too (see ReserveMovedText() in html-parse.cpp). The attributes of an element and the buckets of the tables of names
// of a document are bounded the same way (see below).

// The maximum number of open elements, <html> included: the depth limit of browsers. A start tag at this depth first
// closes the current element, as if its end tag preceded it: deeper elements become siblings instead of children.
inline constexpr std::size_t kMaxOpenElements = 512;

// The maximum number of entries of the list of active formatting elements after its last marker (that is, in the
// current table cell, template...). Beyond it, the earliest entries are removed, as the Noah's Ark clause of HTML
// removes the earliest of 4 identical ones: they are no longer recreated.
inline constexpr std::size_t kMaxActiveFormattingElements = 16;

// The maximum number of attributes that the parsing gives an element. The tree builder looks each attribute of a start
// tag up among those its element already has (a list), so that a tag with many attributes, or many <html> or <body>
// tags adding theirs to the same element, took quadratic time: 65536 attributes in one tag (440 KB) took 4 s. A start
// tag keeps its first attributes up to the limit, duplicates included, an <html> or <body> tag adding its attributes
// to an existing element only as many as this element can still take; the others are ignored.
inline constexpr std::size_t kMaxAttributes = 256;

// The tables of a document interning the names of its tags and attributes grow beyond this many names per bucket on
// average. lexbor creates them with 128 buckets and never grows them: each distinct name made the lookups of the others
// longer, in quadratic time (2.7 MB of "<i aN>", each N different, took 7 s).
inline constexpr std::size_t kMaxNamesPerBucket = 4;

// The parsing stops, ignoring the rest of its input, when a bucket of these tables holds more than this many names.
// The hash function of lexbor has no seed: names of the same hash are easy to compute, and they all go to the same
// bucket whatever the size of its table, each new one compared to all the others (16384 such names in a tag, 900 KB,
// took 1.1 s, 4 times as many would take 16 times longer). Other names never fill a bucket that much: 4 million names
// of usual shapes ("a<N>", "x-<N>", "data-v-<hex>"...) put at most 18 names in a bucket at 4 per bucket on average.
inline constexpr std::size_t kMaxCollidingNames = 64;

// The parsing reads its input in chunks of this many bytes, growing the tables of names and checking their buckets
// after each: the tokenizer interns the names of a tag as it reads them, long before the tree builder gets the tag.
inline constexpr std::size_t kParseChunkSize = 16UL * 1024;

// A parsing first gives these tables a bucket per kHtmlBytesPerNameBucket bytes of its input: each growth hashes all
// the names again, and an input of up to a name per 64 bytes needs none.
inline constexpr std::size_t kHtmlBytesPerNameBucket = 256;

// Gives 'table', a table of lexbor interning names, 'bucketCount' buckets, more than it has, keeping its entries.
// Returns false on memory exhaustion, the table staying as it was.
[[nodiscard]] bool GrowNameTable(lexbor_hash& table, std::size_t bucketCount) noexcept;

// Parses 'html' (UTF-8) into pDocument, a new document, with the limits above.
// Throws std::bad_alloc on memory exhaustion.
void ParseHtml(lxb_html_document* pDocument, std::string_view html);

// Parses 'html' (UTF-8) as a fragment of pDocument, a parsed document, in the context of an element of tag 'contextTag'
// in namespace 'contextNs' (lxb_tag_id_t and lxb_ns_id_t, the HTML fragment parsing algorithm), with the limits above:
// the depth counts from the fragment, and only the buckets of the names it adds are checked. Returns an <html> element
// of pDocument out of the tree, whose children are the nodes of the fragment. Throws std::bad_alloc on memory
// exhaustion.
lxb_dom_node* ParseHtmlFragment(lxb_html_document* pDocument, std::uintptr_t contextTag, std::uintptr_t contextNs,
                                std::string_view html);

}  // namespace aeronet::internal
