#include "aeronet/multipart-form-data.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

std::string BuildBody(std::initializer_list<std::string_view> segments) {
  std::string body;
  for (auto segment : segments) {
    body.append(segment);
  }
  return body;
}

void ExpectInvalid(const MultipartFormData& form, std::string_view invalidReason) {
  EXPECT_FALSE(form.valid());
  EXPECT_EQ(form.invalidReason(), invalidReason);
}

}  // namespace

TEST(MultipartFormDataTest, DefaultConstructorCreatesEmpty) {
  MultipartFormData form;
  EXPECT_TRUE(form.valid());
  EXPECT_TRUE(form.empty());
  EXPECT_TRUE(form.parts().empty());
  EXPECT_TRUE(form.invalidReason().empty());
}

TEST(MultipartFormDataTest, ParsesTextAndFileParts) {
  const std::string body = BuildBody({
      "--TestBoundary\r\n",
      "Content-Disposition: form-data; name=\"field1\"\r\n",
      "\r\n",
      "value-1\r\n",
      "--TestBoundary\r\n",
      "Content-Disposition: form-data; name=\"file\"; filename=\"hello.txt\"\r\n",
      "Content-Type: text/plain\r\n",
      "\r\n",
      "file-content\r\n",
      "--TestBoundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=TestBoundary", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 2);
  EXPECT_FALSE(form.empty());

  const auto& textPart = form.parts()[0];
  EXPECT_EQ(textPart.name, "field1");
  EXPECT_FALSE(textPart.filename.has_value());
  EXPECT_FALSE(textPart.contentType.has_value());
  EXPECT_EQ(textPart.value, "value-1");

  const auto& filePart = form.parts()[1];
  EXPECT_EQ(filePart.name, "file");
  ASSERT_TRUE(filePart.filename.has_value());
  EXPECT_EQ(filePart.filename.value_or(std::string_view{}), "hello.txt");
  ASSERT_TRUE(filePart.contentType.has_value());
  EXPECT_EQ(filePart.contentType.value_or(std::string_view{}), "text/plain");
  EXPECT_EQ(filePart.value, "file-content");
  EXPECT_EQ(filePart.headerValueOrEmpty(http::ContentType), "text/plain");
}

TEST(MultipartFormDataTest, QuotedBoundaryAndLookupByName) {
  const std::string body = BuildBody({
      "--Aa--123\r\n",
      "Content-Disposition: form-data; name=\"alpha\"\r\n",
      "\r\n",
      "a\r\n",
      "--Aa--123\r\n",
      "Content-Disposition: form-data; name=\"alpha\"; filename=\"b.txt\"\r\n",
      "Content-Type: application/octet-stream\r\n",
      "\r\n",
      "xyz\r\n",
      "--Aa--123--\r\n",
  });

  auto form = MultipartFormData("multipart/form-data; boundary=\"Aa--123\"", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 2);

  const auto* first = form.part("alpha");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->value, "a");

  auto allAlpha = form.parts("alpha");
  ASSERT_EQ(allAlpha.size(), 2);
  EXPECT_EQ(allAlpha[1].get().filename.value_or(std::string_view{}), "b.txt");
}

TEST(MultipartFormDataTest, PartLookupGracefullyHandlesMissingNames) {
  const std::string body = BuildBody({
      "--LookupBoundary\r\n",
      "Content-Disposition: form-data; name=\"alpha\"\r\n",
      "\r\n",
      "a\r\n",
      "--LookupBoundary--\r\n",
  });

  auto form = MultipartFormData("multipart/form-data; boundary=LookupBoundary", body);
  ASSERT_TRUE(form.valid());
  EXPECT_EQ(form.part("beta"), nullptr);
  EXPECT_TRUE(form.parts("beta").empty());
  EXPECT_TRUE(form.parts()[0].headerValueOrEmpty("Missing").empty());
}

TEST(MultipartFormDataTest, DefaultPartExposesNoHeaders) {
  vector<MultipartHeaderView> headers;
  MultipartFormData::Part part(headers);
  EXPECT_TRUE(part.name.empty());
  EXPECT_TRUE(part.headers().empty());
  EXPECT_TRUE(part.headerValueOrEmpty("anything").empty());
}

TEST(MultipartFormDataTest, FilenameStarParameterIsHandled) {
  const std::string body = BuildBody({
      "--Utf8Boundary\r\n",
      "Content-Disposition: form-data; name=\"upload\"; filename*=utf-8''sample.bin\r\n",
      "Content-Type: application/octet-stream\r\n",
      "\r\n",
      "payload\r\n",
      "--Utf8Boundary--\r\n",
  });

  auto form = MultipartFormData("multipart/form-data; boundary=Utf8Boundary", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 1);
  const auto& part = form.parts()[0];
  ASSERT_TRUE(part.filename.has_value());
  EXPECT_EQ(part.filename.value_or(std::string_view{}), "sample.bin");
  ASSERT_TRUE(part.contentType.has_value());
  EXPECT_EQ(part.contentType.value_or(std::string_view{}), "application/octet-stream");
}

TEST(MultipartFormDataTest, MissingBoundaryMakesFormInvalid) {
  MultipartFormData form("multipart/form-data", "");
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, EmptyContentTypeHeaderMakesFormInvalid) {
  MultipartFormData form({}, "");
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, EmptyParamsMakeFormInvalid) {
  const std::string body = BuildBody({
      "--Test--Boundary\r\n",
      " \t\tContent-Disposition  \t:\t  form-data \t ; \t name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Test--Boundary--\r\n",
  });

  MultipartFormData form("multipart/form-data;", body);
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, EmptyParamsMakeFormInvalid2) {
  const std::string body = BuildBody({
      "--Test--Boundary\r\n",
      " \t\tContent-Disposition  \t:\t  form-data \t ; \t name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Test--Boundary--\r\n",
  });

  MultipartFormData form("multipart/form-data;;;", body);
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, BoundaryTypeMustMatch) {
  const std::string body = BuildBody({"--Mismatch\r\n"});
  MultipartFormData form("multipart/mixed; boundary=Mismatch", body);
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, MiddlePrefixSpaces) {
  const std::string body = BuildBody({
      "--Test--Boundary\r\n",
      " \t\tContent-Disposition  \t:\t  form-data \t ; \t name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Test--Boundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Test--Boundary", body);

  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 1);
  EXPECT_EQ(form.parts()[0].name, "a");
  EXPECT_EQ(form.parts()[0].value, "1");
}

TEST(MultipartFormDataTest, BoundaryTypo) {
  const std::string body = BuildBody({
      "--Test--Boundary\r\n",
      " \t\tContent-Disposition  \t:\t  form-data \t ; \t name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Test--Boundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boudary=Test--Boundary", body);
  ExpectInvalid(form, "multipart/form-data boundary missing");
}

TEST(MultipartFormDataTest, MaxPartsConfiguration) {
  const std::string body = BuildBody({
      "--TestBoundary\r\n",
      "Content-Disposition: form-data; name= \"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--TestBoundary\r\n",
      "Content-Disposition: form-data; name\t=\"b\"\r\n",
      "\r\n",
      "2\r\n",
      "--TestBoundary--\r\n",
  });

  MultipartFormDataOptions options;
  options.maxParts = 1;
  MultipartFormData form("multipart/form-data; boundary=TestBoundary", body, options);
  ExpectInvalid(form, "multipart exceeds part limit");

  options.maxParts = 0;  // no limit
  MultipartFormData form2("multipart/form-data; boundary=TestBoundary", body, options);
  ASSERT_TRUE(form2.valid());
  ASSERT_EQ(form2.parts().size(), 2);
}

TEST(MultipartFormDataTest, MissingContentDispositionRejected) {
  const std::string body = BuildBody({
      "--TestBoundary\r\n",
      "X-Test: demo \r\n",
      "\r\n",
      "no header\r\n",
      "--TestBoundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=TestBoundary", body);
  ExpectInvalid(form, "multipart part missing Content-Disposition header");
}

TEST(MultipartFormDataTest, EmptyLine) {
  const std::string body = BuildBody({
      "--TestBoundary\r\n\r\n",
      " \r\n",
      "\r\n",
      "no header\r\n",
      "--TestBoundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=TestBoundary", body);
  ExpectInvalid(form, "multipart part header missing colon");
}

TEST(MultipartFormDataTest, ContentDispositionMustContainValue) {
  const std::string body = BuildBody({
      "--CDValue\r\n",
      "Content-Disposition:\r\n",
      "\r\n",
      "value\t\r\n",
      "--CDValue--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=CDValue", body);
  ExpectInvalid(form, "multipart part missing Content-Disposition value");
}

TEST(MultipartFormDataTest, ContentDispositionTypeMustBeFormData) {
  const std::string body = BuildBody({
      "--CDType\r\n",
      "Content-Disposition: attachment; name=\"field\"\r\n",
      "\r\n",
      "value\r\n",
      "--CDType--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=CDType", body);
  ExpectInvalid(form, "multipart part must have Content-Disposition: form-data");
}

TEST(MultipartFormDataTest, ContentDispositionRequiresNameParameter) {
  const std::string body = BuildBody({
      "--CDName\r\n",
      "Content-Disposition: form-data; filename=\"f.txt\"\r\n",
      "\r\n",
      "value\r\n",
      "--CDName--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=CDName", body);
  ExpectInvalid(form, "multipart part missing name parameter");
}

TEST(MultipartFormDataTest, BareContentDispositionParameterIsInvalid) {
  const std::string body = BuildBody({
      "--boundary\r\n",
      "Content-Disposition: form-data; name\r\n",
      "\r\n",
      "value\r\n",
      "--boundary--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=boundary", body);
  ExpectInvalid(form, "multipart part invalid Content-Disposition parameter");
}

TEST(MultipartFormDataTest, EmptyContentDispositionTokenIsInvalid) {
  const std::string body = BuildBody({
      "--EmptyTok\r\n",
      "Content-Disposition: form-data;; name=\"a\"\r\n",
      "\r\n",
      "value\r\n",
      "--EmptyTok--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=EmptyTok", body);
  ExpectInvalid(form, "multipart part invalid Content-Disposition parameter");
}

TEST(MultipartFormDataTest, MalformedFilenameStarIsInvalid1) {
  const std::string body = BuildBody({
      "--Fs\r\n",
      "Content-Disposition: form-data; filename*=utf-8langvalue\r\n",
      "\r\n",
      "payload\r\n",
      "--Fs--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Fs", body);
  ExpectInvalid(form, "multipart part invalid Content-Disposition filename* parameter");
}

TEST(MultipartFormDataTest, MalformedFilenameStarIsInvalid2) {
  const std::string body = BuildBody({
      "--Fs\r\n",
      "    \t\t Content-Disposition \t\t\t : form-data; filename*=utf-8'langvalue\r\n",
      "\r\n",
      "payload\r\n",
      "--Fs--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Fs", body);
  ExpectInvalid(form, "multipart part invalid Content-Disposition filename* parameter");
}

TEST(MultipartFormDataTest, MalformedFilenameStarIsInvalid3) {
  const std::string body = BuildBody({
      "--Fs\r\n",
      "    \t\t Content-Disposition \t\t\t : form-data; filename*=utf-8'langvalue'\r\n",
      "\r\n",
      "payload\r\n",
      "--Fs--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Fs", body);
  ExpectInvalid(form, "multipart part invalid Content-Disposition filename* parameter");
}

TEST(MultipartFormDataTest, StartingBoundaryMustExist) {
  const std::string body = BuildBody({"garbage"});
  MultipartFormData form("multipart/form-data; boundary=Start", body);
  ExpectInvalid(form, "multipart body missing starting boundary");
}

TEST(MultipartFormDataTest, StartingBoundaryMustExist2) {
  const std::string body = BuildBody({"--garbage"});
  MultipartFormData form("multipart/form-data; boundary=toto", body);
  ExpectInvalid(form, "multipart body missing starting boundary");
}

TEST(MultipartFormDataTest, BoundaryMustBeFollowedByCRLF) {
  const std::string body = BuildBody({"--NoCrlf"});
  MultipartFormData form("multipart/form-data; boundary=NoCrlf", body);
  ExpectInvalid(form, "multipart boundary not followed by CRLF");
}

TEST(MultipartFormDataTest, MissingHeaderTerminatorThrows) {
  const std::string body = BuildBody({
      "--NoHeaderTerminator\r\n",
      "Content-Disposition: form-data; name=\"field\"",
  });

  MultipartFormData form("multipart/form-data; boundary=NoHeaderTerminator", body);
  ExpectInvalid(form, "multipart part missing header terminator");
}

TEST(MultipartFormDataTest, HeaderMustContainColon) {
  const std::string body = BuildBody({
      "--NoColon\r\n",
      "Content-Disposition form-data  \t \t \t\t  ;    \t\t\t  name=\"field\"\r\n",
      "\r\n",
      "value\r\n",
      "--NoColon--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=NoColon", body);
  ExpectInvalid(form, "multipart part header missing colon");
}

TEST(MultipartFormDataTest, HeaderMustContainName) {
  const std::string body = BuildBody({
      "--NoName\r\n",
      ": missing name\r\n",
      "\r\n",
      "value\r\n",
      "--NoName--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=NoName", body);
  ExpectInvalid(form, "multipart part header missing name");
}

TEST(MultipartFormDataTest, HeaderLimitIsEnforced) {
  const std::string body = BuildBody({
      "--HeaderLimit\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "Content-Type: text/plain\r\n",
      "\r\n",
      "v\r\n",
      "--HeaderLimit--\r\n",
  });

  MultipartFormDataOptions options;
  options.maxHeadersPerPart = 1;
  MultipartFormData form("multipart/form-data; boundary=HeaderLimit", body, options);
  ExpectInvalid(form, "multipart part exceeds header limit");

  options.maxHeadersPerPart = 0;  // no limit
  MultipartFormData form2("multipart/form-data; boundary=HeaderLimit", body, options);
  ASSERT_TRUE(form2.valid());
  ASSERT_EQ(form2.parts().size(), 1);
  EXPECT_EQ(form2.parts()[0].headerValueOrEmpty(http::ContentType), "text/plain");
}

TEST(MultipartFormDataTest, MissingClosingBoundaryInvalid) {
  const std::string body = BuildBody({
      "--NoClosing\r\n",
      "Content-Disposition: form-data; name=\"field\"\r\n",
      "\r\n",
      "value",
  });

  MultipartFormData form("multipart/form-data; boundary=NoClosing", body);
  ExpectInvalid(form, "multipart part missing closing boundary");
}

TEST(MultipartFormDataTest, MissingClosingBoundaryInvalid2) {
  const std::string body = BuildBody({
      "--NoClosing\r\n",
      "Content-Disposition: form-data; name=\"field\"\r\n",
      "\r\n",
      "value",
      "\r\n--Another--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=NoClosing", body);
  ExpectInvalid(form, "multipart part missing closing boundary");
}

TEST(MultipartFormDataTest, PartSizeLimitIsHonored) {
  const std::string body = BuildBody({
      "--PartLimit\r\n",
      "Content-Disposition: form-data; name=\"field\"\r\n",
      "\r\n",
      "oversize\r\n",
      "--PartLimit--\r\n",
  });

  MultipartFormDataOptions options;
  options.maxPartSizeBytes = 4;
  MultipartFormData form("multipart/form-data; boundary=PartLimit", body, options);
  ExpectInvalid(form, "multipart part exceeds size limit");

  options.maxPartSizeBytes = 0;  // no limit
  MultipartFormData form2("multipart/form-data; boundary=PartLimit", body, options);
  ASSERT_TRUE(form2.valid());
  ASSERT_EQ(form2.parts().size(), 1);
  EXPECT_EQ(form2.parts()[0].value, "oversize");
}

TEST(MultipartFormDataTest, BoundaryRequiresTrailingCRLFForNextPart) {
  const std::string body = BuildBody({
      "--Multi\r\n",
      "Content-Disposition: form-data; name=\"first\"\r\n",
      "\r\n",
      "1\r\n",
      "--Multi",
      "Content-Disposition: form-data; name=\"second\"\r\n",
      "\r\n",
      "2\r\n",
      "--Multi--\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Multi", body);
  ExpectInvalid(form, "multipart boundary missing CRLF");
}

TEST(MultipartFormDataTest, FinalBoundaryNoTrailingCRLFAccepted) {
  // Create a body that has a single part and then the final boundary marker
  // with no trailing CRLF. At the check in multipart-form-data.cpp this
  // should make `body.starts_with(http::CRLF)` false, and `!body.empty() &&
  // !finalBoundary` false as well (because it is a final boundary), so the
  // parser should accept the final boundary and succeed.
  const std::string body = BuildBody({
      "--Final\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Final--",  // final boundary with no trailing CRLF
  });

  MultipartFormData form("multipart/form-data; boundary=Final", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 1);
  EXPECT_EQ(form.parts()[0].value, "1");
}

TEST(MultipartFormDataTest, FinalBoundaryWithTrailingDataRejected) {
  // Construct a body that contains a final boundary but with extra trailing
  // data (not just an optional trailing CRLF). This should make
  // `body.starts_with(http::CRLF)` false, `!body.empty()` true, and
  // `!finalBoundary` false (since it is a final boundary). The parser should
  // therefore reject with "multipart data after final boundary".
  const std::string body = BuildBody({
      "--Tail\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Tail--",
      "EXTRA",
  });

  MultipartFormData form("multipart/form-data; boundary=Tail", body);
  ExpectInvalid(form, "multipart data after final boundary");
}

TEST(MultipartFormDataTest, FinalBoundaryWithEmptyTrailingDataAccepted) {
  // Construct a body that contains a final boundary but with extra trailing
  // data (not just an optional trailing CRLF). This should make
  // `body.starts_with(http::CRLF)` false, `!body.empty()` true, and
  // `!finalBoundary` false (since it is a final boundary). The parser should
  // therefore reject with "multipart data after final boundary".
  const std::string body = BuildBody({
      "--Tail\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--Tail--",
      "\r\n\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=Tail", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 1);
  EXPECT_EQ(form.parts()[0].value, "1");
}

TEST(MultipartFormDataTest, InvalidHeadersEnd) {
  const std::string body = BuildBody({
      "--Tail\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "\r ",
      "1\r\n",
      "--Tail--",
      "\r\n \n",
  });

  MultipartFormData form("multipart/form-data; boundary=Tail", body);
  ExpectInvalid(form, "multipart data after final boundary");
}

TEST(MultipartFormDataTest, InvalidHeadersEnd2) {
  const std::string body = BuildBody({"--Tail\r\n", "Content-Disposition: form-data; name=\"a\"\r\n", "\r"});

  MultipartFormData form("multipart/form-data; boundary=Tail", body);
  ExpectInvalid(form, "multipart part missing closing boundary");
}

TEST(MultipartFormDataTest, StripQuotes) {
  // Construct a body that contains a final boundary but with extra trailing
  // data (not just an optional trailing CRLF). This should make
  // `body.starts_with(http::CRLF)` false, `!body.empty()` true, and
  // `!finalBoundary` false (since it is a final boundary). The parser should
  // therefore reject with "multipart data after final boundary".
  const std::string body = BuildBody({
      "--A\r\n",
      "Content-Disposition: form-data; name=a; value=\"b; something=c\"; data=\"d\"\r\n",
      "\r\n",
      "1\r\n",
      "--A--",
      "\r\n\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=A", body);
  ASSERT_TRUE(form.valid());
  ASSERT_EQ(form.parts().size(), 1);
  EXPECT_EQ(form.parts()[0].value, "1");
  const auto& part = form.parts()[0];
  EXPECT_EQ(part.name, "a");
  EXPECT_EQ(part.headerValueOrEmpty(http::ContentDisposition),
            "form-data; name=a; value=\"b; something=c\"; data=\"d\"");
}

TEST(MultipartFormDataTest, EmptyParseContentDisposition) {
  // Construct a body that contains a final boundary but with extra trailing
  // data (not just an optional trailing CRLF). This should make
  // `body.starts_with(http::CRLF)` false, `!body.empty()` true, and
  // `!finalBoundary` false (since it is a final boundary). The parser should
  // therefore reject with "multipart data after final boundary".
  const std::string body = BuildBody({
      "--A\r\n",
      "Content-Disposition: form-data;\r\n",
      "\r\n",
      "1\r\n",
      "--A--",
      "\r\n\r\n",
  });

  MultipartFormData form("multipart/form-data; boundary=A", body);
  ExpectInvalid(form, "multipart part missing name parameter");
}

TEST(MultipartFormDataTest, DataAfterFinalBoundaryIsRejected) {
  const std::string body = BuildBody({
      "--After\r\n",
      "Content-Disposition: form-data; name=\"a\"\r\n",
      "\r\n",
      "1\r\n",
      "--After--\r\n",
      "trailing",
  });

  MultipartFormData form("multipart/form-data; boundary=After", body);
  ExpectInvalid(form, "multipart data after final boundary");
}

}  // namespace aeronet
