/*
 * Copyright 2012 Google Inc.
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
// Author: nikhilmadan@google.com (Nikhil Madan)

#include "net/instaweb/rewriter/public/flush_early_content_writer_filter.h"

#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/flush_early_info_finder_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "testing/base/public/gunit.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/wildcard.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/user_agent_matcher_test_base.h"
#include "pagespeed/opt/logging/enums.pb.h"

namespace net_instaweb {

const char kMockHashValue[] = "MDAwMD";

const char kPrefetchScript[] =
    "<script type='text/javascript'>window.mod_pagespeed_prefetch_start"
    " = Number(new Date());window.mod_pagespeed_num_resources_prefetched"
    " = %d</script>";

const char kHtmlInputPublicCacheableResources[] =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"f.css\"/>"
      "<script src=\"g.js\"></script>"
      "<script src=\"http://www.test.com/h.js.pagespeed.jm.%s.js\"></script>"
    "</head>"
    "<body></body></html>";

const char kHtmlInputPrivateCacheableResources[] =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\"/>"
      "<script src=\"b.js\"></script>"
      "<script src=\"http://www.test.com/c.js.pagespeed.jm.%s.js\"></script>"
      "<link type=\"text/css\" rel=\"stylesheet\" href="
      "\"d.css.pagespeed.cf.%s.css\"/>"
    "</head>"
    "<body></body></html>";

class FlushEarlyContentWriterFilterTest : public RewriteTestBase {
 public:
  FlushEarlyContentWriterFilterTest() : writer_(&output_) {}

  virtual bool AddHtmlTags() const { return false; }

 protected:
  virtual void SetUp() {
    SetMockHashValue("00000");  // Base64 encodes to kMockHashValue.
    SetResponseWithDefaultHeaders(
        "http://test.com/a.css", kContentTypeCss,
        ".a { background-color: blue; }", 100 /* sec */);
    SetResponseWithDefaultHeaders(
        "http://test.com/f.css", kContentTypeCss,
        ".f { background-color: red; }", 100 /* sec */);
    SetResponseWithDefaultHeaders(
        "http://test.com/css", kContentTypeCss,
        ".c { background-color: green; }", 100 /* sec */);
    SetResponseWithDefaultHeaders(
        "http://test.com/b.js", kContentTypeJavascript,
        "alert('foo')", 100 /* sec */);
    SetResponseWithDefaultHeaders(
        "http://test.com/g.js", kContentTypeJavascript,
        "alert('bar')", 100 /* sec */);
    statistics()->AddTimedVariable(
      FlushEarlyContentWriterFilter::kNumResourcesFlushedEarly,
      Statistics::kDefaultGroup);
    options()->EnableFilter(RewriteOptions::kFlushSubresources);
    options()->set_flush_more_resources_early_if_time_permits(true);
    options()->set_flush_more_resources_in_ie_and_firefox(true);
    RewriteTestBase::SetUp();
    rewrite_driver()->set_flushing_early(true);
    rewrite_driver()->AddFilters();
    rewrite_driver()->SetWriter(&writer_);
    server_context()->set_flush_early_info_finder(
        new MeaningfulFlushEarlyInfoFinder);
    rewrite_driver_->log_record()->SetLogUrlIndices(true);
  }

  virtual void Clear() {
    ClearRewriteDriver();
    rewrite_driver_->flush_early_info()->set_average_fetch_latency_ms(190);
    rewrite_driver_->log_record()->SetLogUrlIndices(true);
    output_.clear();
    rewrite_driver()->SetWriter(&writer_);
  }

  void ResetUserAgent(StringPiece user_agent) {
    Clear();
    SetCurrentUserAgent(user_agent);
    SetDriverRequestHeaders();
  }

  void EnableDeferJsAndSetFetchLatency(int latency) {
    Clear();
    options()->ClearSignatureForTesting();
    options()->EnableFilter(RewriteOptions::kDeferJavascript);
    server_context()->ComputeSignature(options());
    rewrite_driver_->flush_early_info()->set_average_fetch_latency_ms(latency);
  }

  GoogleString RewrittenOutputWithResources(const GoogleString& html_output,
                                            const int& number_of_resources,
                                            bool links_flushed) {
    return StrCat(html_output,
                  (links_flushed ?
                   FlushEarlyContentWriterFilter::kDisableLinkTag :
                   ""),
                  StringPrintf(kPrefetchScript, number_of_resources));
  }

  void ExpectNumLogRecords(int expected_records) {
    ScopedMutex lock(rewrite_driver_->log_record()->mutex());
    EXPECT_EQ(expected_records,
              rewrite_driver_->log_record()->logging_info()
              ->rewriter_info_size());
  }

  void ExpectAvailableTimeMs(int64 expected_available_time_ms) {
    ScopedMutex lock(rewrite_driver_->log_record()->mutex());
    EXPECT_EQ(expected_available_time_ms,
              rewrite_driver_->log_record()->logging_info()
              ->flush_early_flow_info().available_time_ms());
  }

  void ExpectLogRecord(int index,
                       RewriterApplication::Status status,
                       int resource_index,
                       FlushEarlyResourceInfo::ContentType content_type,
                       FlushEarlyResourceInfo::ResourceType resource_type,
                       bool is_bandwidth_affected,
                       bool in_head) {
    const RewriterInfo& rewriter_info =
        rewrite_driver_->log_record()->logging_info()->rewriter_info(index);
    const FlushEarlyResourceInfo& resource_info =
        rewriter_info.flush_early_resource_info();
    EXPECT_EQ("fs", rewriter_info.id());
    EXPECT_EQ(status, rewriter_info.status());
    if (resource_index >= 0) {
      EXPECT_TRUE(rewriter_info.has_rewrite_resource_info());
      EXPECT_EQ(resource_index,
          rewriter_info.rewrite_resource_info().original_resource_url_index());
    } else {
      EXPECT_FALSE(rewriter_info.has_rewrite_resource_info());
    }
    EXPECT_EQ(content_type, resource_info.content_type());
    EXPECT_EQ(resource_type, resource_info.resource_type());
    EXPECT_EQ(is_bandwidth_affected, resource_info.is_bandwidth_affected());
    EXPECT_EQ(in_head, resource_info.in_head());
  }

  void SetPrivateCacheableUrls() {
    FlushEarlyRenderInfo* info =  new FlushEarlyRenderInfo;
    info->add_private_cacheable_url("http://test.com/a.css");
    info->add_private_cacheable_url("http://test.com/c.js");
    info->add_private_cacheable_url("http://test.com/d.css");
    rewrite_driver()->set_flush_early_render_info(info);
  }

  void SetPublicCacheableUrls() {
    FlushEarlyRenderInfo* info =  new FlushEarlyRenderInfo;
    info->add_public_cacheable_url("http://test.com/f.css");
    info->add_public_cacheable_url("http://test.com/g.js");
    rewrite_driver()->set_flush_early_render_info(info);
  }

  GoogleString GetOutputWithHash(StringPiece format) {
    GoogleString output(format.data(), format.size());
    GlobalReplaceSubstring("%s", kMockHashValue, &output);
    return output;
  }

  void VerifyJsNotFlushed() {
    SetPrivateCacheableUrls();
    GoogleString html_input = GetOutputWithHash(
        kHtmlInputPrivateCacheableResources);
    GoogleString html_output;

    html_output = GetOutputWithHash(
        "<link rel=\"stylesheet\" href=\"a.css\"/>\n"
        "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n");

    Parse("prefetch_image_tag", html_input);
    EXPECT_EQ(RewrittenOutputWithResources(html_output, 2, true), output_);
  }

  GoogleString output_;

 private:
  scoped_ptr<FlushEarlyContentWriterFilter> filter_;
  StringWriter writer_;

  DISALLOW_COPY_AND_ASSIGN(FlushEarlyContentWriterFilterTest);
};

TEST_F(FlushEarlyContentWriterFilterTest, TestDifferentBrowsers) {
  Clear();
  GoogleString html_input = GetOutputWithHash(
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\" "
        "data-pagespeed-size=\"1000\"/>"
        "<script src=\"b.js\" data-pagespeed-size=\"1000\"></script>"
        "<script src=\"http://www.test.com/c.js.pagespeed.jm.%s.js\" "
        "data-pagespeed-size=\"1000\"></script>"
        "<link type=\"text/css\" rel=\"stylesheet\" href="
        "\"d.css.pagespeed.cf.%s.css\" data-pagespeed-size=\"1000\"/>"
        "<img src=\"http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\" "
        "data-pagespeed-size=\"1000\"/>"
        "<img src=\"http://www.test.com/g.jpg.pagespeed.ce.%s.jpg\" "
        "data-pagespeed-size=\"1000000\"/>"
        "<link rel=\"dns-prefetch\" href=\"//test.com\">"
        "<link rel=\"prefetch\" href=\"//test1.com\">"
      "</head>"
      "<body>"
      "<script src=\"d.js.pagespeed.ce.%s.js\" "
      "data-pagespeed-size=\"1000\"></script>"
      "<script src=\"e.js.pagespeed.ce.%s.js\" "
      "data-pagespeed-size=\"100000\"></script>"
      "</body></html>");
  GoogleString html_output;

  // First test with no User-Agent.
  Parse("no_user_agent", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);
  // TODO(mmohabey): If the browser is not supported by flush subresources
  // filter, we should avoid all the code in StartDocument/EndDocument.
  // Otherwise we will be creating spurious log records like below.
  ExpectNumLogRecords(1);
  ExpectAvailableTimeMs(190);
  // DeferJs script is not flushed since it does not support the empty
  // user-agent.
  ExpectLogRecord(0,
                  RewriterApplication::NOT_APPLIED,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  false /* not affected by bandwidth */,
                  false /* not in HEAD */);

  // Set the User-Agent to prefetch_link_script_tag.
  ResetUserAgent("prefetch_link_script_tag");
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){new Image().src=\""
      "http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\";})()</script>"
      "<link rel=\"dns-prefetch\" href=\"//test.com\">"
      "<link rel=\"prefetch\" href=\"//test1.com\">"
      "<script type=\"psa_prefetch\" src="
      "\"http://www.test.com/c.js.pagespeed.jm.%s.js\"></script>\n"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n"
      "<script type=\"psa_prefetch\" src=\"d.js.pagespeed.ce.%s.js\">"
      "</script>\n");

  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 4, true), output_);

  ExpectNumLogRecords(9);
  ExpectAvailableTimeMs(190);
  // a.css is non-rewritten CSS.
  ExpectLogRecord(0,
                  RewriterApplication::NOT_APPLIED,
                  0,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::NON_PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // b.js is non-rewritten JS.
  ExpectLogRecord(1,
                  RewriterApplication::NOT_APPLIED,
                  1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::NON_PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // c.js is rewritten JS.
  ExpectLogRecord(2,
                  RewriterApplication::APPLIED_OK,
                  2,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // d.css is rewritten CSS.
  ExpectLogRecord(3,
                  RewriterApplication::APPLIED_OK,
                  3,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // e.jpg is an image and the prefetch mechanism does not allow flushing
  // images.
  ExpectLogRecord(4,
                  RewriterApplication::APPLIED_OK,
                  4,
                  FlushEarlyResourceInfo::IMAGE,
                  FlushEarlyResourceInfo::PAGESPEED,
                  true /* not affected by bandwidth */,
                  true /* in HEAD */);
  // g.jpg is an image but size does not allow it to flushing.
  ExpectLogRecord(5,
                  RewriterApplication::NOT_APPLIED,
                  5,
                  FlushEarlyResourceInfo::IMAGE,
                  FlushEarlyResourceInfo::PAGESPEED,
                  true /* not affected by bandwidth */,
                  true /* in HEAD */);
  // d.js is rewritten JS.
  ExpectLogRecord(6,
                  RewriterApplication::APPLIED_OK,
                  6,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  true /* not affected by bandwidth */,
                  false /* not in HEAD */);
  // e.js is rewritten JS but size exceeds threshold.
  ExpectLogRecord(7,
                  RewriterApplication::NOT_APPLIED,
                  7,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  true /* not affected by bandwidth */,
                  false /* not in HEAD */);
  // DeferJs script is not flushed since it is not enabled. SetUp is not called
  // again with the test case, so many of the filters/options are actually
  // disabled.
  ExpectLogRecord(8,
                  RewriterApplication::NOT_APPLIED,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  false /* not affected by bandwidth */,
                  false /* not in HEAD */);

  // Set the User-Agent to prefetch_image_tag.
  ResetUserAgent("prefetch_image_tag");
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://www.test.com/c.js.pagespeed.jm.%s.js\";"
      "new Image().src=\"http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\";})()"
      "</script>"
      "<link rel=\"dns-prefetch\" href=\"//test.com\">"
      "<link rel=\"prefetch\" href=\"//test1.com\">"
      "<script type=\"text/javascript\">"
      "(function(){new Image().src=\"d.js.pagespeed.ce.%s.js\";})()</script>"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n");

  Parse("prefetch_image_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 4, true), output_);

  // Enable defer_javasript. We will flush JS resources only if time permits.
  ResetUserAgent("prefetch_image_tag");
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  server_context()->ComputeSignature(options());

  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\";})()"
      "</script>"
      "<link rel=\"dns-prefetch\" href=\"//test.com\">"
      "<link rel=\"prefetch\" href=\"//test1.com\">"
      "<script type=\"text/javascript\">"
      "(function(){"
      "new Image().src=\"http://www.test.com/c.js.pagespeed.jm.%s.js\";"
      "new Image().src=\"d.js.pagespeed.ce.%s.js\";})()</script>"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n");

  Parse("defer_javasript", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 4, true), output_);

  // Set the User-Agent to prefetch_link_script_tag with defer_javascript
  // enabled.
  ResetUserAgent("prefetch_link_script_tag");
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){new Image().src=\""
      "http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\";})()</script>"
      "<link rel=\"dns-prefetch\" href=\"//test.com\">"
      "<link rel=\"prefetch\" href=\"//test1.com\">"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n"
      "<script type=\"psa_prefetch\" src=\"/psajs/js_defer.0.js\">"
      "</script>\n");

  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 3, true), output_);

  // Now test link rel=prefetch support. Images still get new Image()
  // fetching, in hope of decoding them ASAP, too.
  ResetUserAgent(UserAgentMatcherTestBase::kChrome42UserAgent);
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){new Image().src=\""
      "http://www.test.com/e.jpg.pagespeed.ce.%s.jpg\";})()</script>"
      "<link rel=\"dns-prefetch\" href=\"//test.com\">"
      "<link rel=\"prefetch\" href=\"//test1.com\">"
      "<link rel=\"prefetch\" href=\"d.css.pagespeed.cf.%s.css\"/>\n"
      "<link rel=\"prefetch\" href=\""
          "http://www.test.com/c.js.pagespeed.jm.%s.js\"/>\n"
      "<link rel=\"prefetch\" href=\"d.js.pagespeed.ce.%s.js\"/>\n"
      "<link rel=\"prefetch\" href=\"/psajs/js_defer.0.js\"/>\n");

  Parse("prefetch_rel_prefetch_tag_escape", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 5, false), output_);
}

TEST_F(FlushEarlyContentWriterFilterTest, EscapeParanoia) {
  Clear();
  GoogleString html_input =
      GetOutputWithHash(
          "<script src='foo\"bar.js.pagespeed.ce.%s.js'"
          " data-pagespeed-size=\"1000\"></script>"
          "<script src=\"b.js\" data-pagespeed-size=\"1000\"></script>");

  ResetUserAgent("prefetch_image_tag");
  Parse("prefetch_image_tag_escape", html_input);
  EXPECT_NE(GoogleString::npos,
            output_.find("new Image().src=\"foo\\\"bar.js"))
      << output_;

  ResetUserAgent("prefetch_link_script_tag");
  Parse("prefetch_link_script_tag_escape", html_input);
  EXPECT_NE(GoogleString::npos,
            output_.find("<script type=\"psa_prefetch\" "
                         "src=\"foo&quot;bar.js."))
      << output_;

  html_input = GetOutputWithHash(
      "<link rel=stylesheet href='foo\"bar.css.pagespeed.ce.%s.css'"
      " data-pagespeed-size=\"1000\">");
  ResetUserAgent("prefetch_link_script_tag");
  Parse("prefetch_link_tag_escape", html_input);
  EXPECT_NE(GoogleString::npos,
            output_.find("<link rel=\"stylesheet\" href=\"foo&quot;bar."))
      << output_;

  html_input = GetOutputWithHash(
      "<link rel=stylesheet href='foo\"bar.css.pagespeed.ce.%s.css'"
      " data-pagespeed-size=\"1000\">");
  ResetUserAgent(UserAgentMatcherTestBase::kChrome42UserAgent);
  Parse("prefetch_rel_prefetch_tag_escape", html_input);
  EXPECT_NE(GoogleString::npos,
            output_.find("<link rel=\"prefetch\" href=\"foo&quot;bar."))
      << output_;
}

TEST_F(FlushEarlyContentWriterFilterTest, NoResourcesToFlush) {
  GoogleString html_input =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\"/>"
        "<script src=\"b.js\"></script>"
      "</head>"
      "<body></body></html>";
  GoogleString html_output;

  // First test with no User-Agent.
  Parse("no_user_agent", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);

  // Set the User-Agent to prefetch_link_script_tag.
  ResetUserAgent("prefetch_link_script_tag");
  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);

  // Set the User-Agent to prefetch_image_tag.
  ResetUserAgent("prefetch_image_tag");
  Parse("prefetch_image_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);
}

TEST_F(FlushEarlyContentWriterFilterTest, TooManyRewriterInfoRecords) {
  Clear();
  GoogleString html_input = GetOutputWithHash(
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
        "<link type=\"text/css\" rel=\"stylesheet\" "
          "href=\"a.css.pagespeed.cf.%s.css\">"
        "<link type=\"text/css\" rel=\"stylesheet\" "
          "href=\"b.css.pagespeed.cf.%s.css\">"
        "<link type=\"text/css\" rel=\"stylesheet\" "
          "href=\"c.css.pagespeed.cf.%s.css\">"
      "</head><body></body></html>");
  GoogleString html_output = GetOutputWithHash(
      "<link rel=\"stylesheet\" href=\"a.css.pagespeed.cf.%s.css\"/>\n"
      "<link rel=\"stylesheet\" href=\"b.css.pagespeed.cf.%s.css\"/>\n"
      "<link rel=\"stylesheet\" href=\"c.css.pagespeed.cf.%s.css\"/>\n");

  ResetUserAgent("prefetch_link_script_tag");
  rewrite_driver_->log_record()->SetRewriterInfoMaxSize(2);
  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 3, true), output_);
  ExpectNumLogRecords(2);
  EXPECT_TRUE(logging_info()->rewriter_info_size_limit_exceeded());
}

TEST_F(FlushEarlyContentWriterFilterTest, FlushDeferJsEarly) {
  GoogleString html_input =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "</head>"
      "<body></body></html>";

  // Set fetch latency to 0.
  // Irrespective of AvailableTimeMs, DeferJs should be flushed early always.
  // User-Agent: prefetch_link_script_tag.
  ResetUserAgent("prefetch_link_script_tag");
  EnableDeferJsAndSetFetchLatency(0);
  SetDriverRequestHeaders();
  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(
      "<script type=\"psa_prefetch\" src=\"/psajs/js_defer.0.js\"></script>\n",
      1, false), output_);

  ExpectNumLogRecords(1);
  ExpectAvailableTimeMs(0);
  ExpectLogRecord(0,
                  RewriterApplication::APPLIED_OK,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  true /* affected by bandwidth */,
                  false /* not in HEAD */);
}

TEST_F(FlushEarlyContentWriterFilterTest, CacheablePrivateResources1) {
  SetPrivateCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPrivateCacheableResources);
  GoogleString html_output;

  // First test with no User-Agent.
  Parse("no_user_agent", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);
}

TEST_F(FlushEarlyContentWriterFilterTest, CacheablePrivateResources2) {
  // Set the User-Agent to prefetch_link_script_tag.
  ResetUserAgent("prefetch_link_script_tag");
  SetPrivateCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPrivateCacheableResources);
  GoogleString html_output;

  html_output = GetOutputWithHash(
      "<link rel=\"stylesheet\" href=\"a.css\"/>\n"
      "<script type=\"psa_prefetch\" src="
      "\"http://www.test.com/c.js.pagespeed.jm.%s.js\"></script>\n"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n");

  Parse("prefetch_link_script_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 3, true), output_);
  ExpectNumLogRecords(5);
  // a.css is private cacheable CSS.
  ExpectLogRecord(0,
                  RewriterApplication::APPLIED_OK,
                  0,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::PRIVATE_CACHEABLE,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // b.js is non-rewritten JS.
  ExpectLogRecord(1,
                  RewriterApplication::NOT_APPLIED,
                  1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::NON_PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // c.js is rewritten JS.
  ExpectLogRecord(2,
                  RewriterApplication::APPLIED_OK,
                  2,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // d.css is rewritten CSS.
  ExpectLogRecord(3,
                  RewriterApplication::APPLIED_OK,
                  3,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // defer_javascript is not enabled.
  ExpectLogRecord(4,
                  RewriterApplication::NOT_APPLIED,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  false /* not affected by bandwidth */,
                  false /* not in HEAD */);
}

TEST_F(FlushEarlyContentWriterFilterTest, CacheablePrivateResources3) {
  // Set the User-Agent to prefetch_image_tag.
  ResetUserAgent("prefetch_image_tag");
  SetPrivateCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPrivateCacheableResources);
  GoogleString html_output;

  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://www.test.com/c.js.pagespeed.jm.%s.js\";"
      "})()</script>"
      "<link rel=\"stylesheet\" href=\"a.css\"/>\n"
      "<link rel=\"stylesheet\" href=\"d.css.pagespeed.cf.%s.css\"/>\n");

  Parse("prefetch_image_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 3, true), output_);
}

TEST_F(FlushEarlyContentWriterFilterTest, CacheablePrivateResources4) {
  // Enable defer_javasript. We don't flush JS resources now.
  ResetUserAgent("prefetch_image_tag");
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  server_context()->ComputeSignature(options());
  VerifyJsNotFlushed();
}

TEST_F(FlushEarlyContentWriterFilterTest, CacheablePublicResources1) {
  SetPublicCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPublicCacheableResources);
  GoogleString html_output;

  // First test with no User-Agent.
  Parse("no_user_agent", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 0, false), output_);
}

TEST_F(FlushEarlyContentWriterFilterTest,
       CacheablePublicResourcesBlacklistedUA) {
  ResetUserAgent("prefetch_image_tag");
  SetPublicCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPublicCacheableResources);
  GoogleString html_output;

  // Disallow one of the public cacheable resources.
  options()->ClearSignatureForTesting();
  options()->Disallow("*f.css*");
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://www.test.com/h.js.pagespeed.jm.%s.js\";"
      "})()</script>"
      "<link rel=\"stylesheet\" href=\"f.css\"/>\n");

  Parse("prefetch_image_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 2, true), output_);
  ExpectNumLogRecords(4);
  // f.css is public cacheable CSS and flush early is applied.
  ExpectLogRecord(0,
                  RewriterApplication::APPLIED_OK,
                  0,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::PUBLIC_CACHEABLE,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // g.js is non-rewritten JS.
  ExpectLogRecord(1,
                  RewriterApplication::NOT_APPLIED,
                  1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PUBLIC_CACHEABLE,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // h.js is rewritten JS.
  ExpectLogRecord(2,
                  RewriterApplication::APPLIED_OK,
                  2,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // defer_javascript is not enabled.
  ExpectLogRecord(3,
                  RewriterApplication::NOT_APPLIED,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  false /* not affected by bandwidth */,
                  false /* not in HEAD */);
}

TEST_F(FlushEarlyContentWriterFilterTest,
       CacheablePublicResourcesNotBlacklistedUA) {
  ResetUserAgent("prefetch_image_tag");
  SetPublicCacheableUrls();
  GoogleString html_input = GetOutputWithHash(
      kHtmlInputPublicCacheableResources);
  GoogleString html_output;

  // Set the User-Agent to prefetch_image_tag.
  html_output = GetOutputWithHash(
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://www.test.com/h.js.pagespeed.jm.%s.js\";"
      "})()</script>");

  Parse("prefetch_image_tag", html_input);
  EXPECT_EQ(RewrittenOutputWithResources(html_output, 1, false), output_);
  ExpectNumLogRecords(4);
  // f.css is public cacheable CSS.
  ExpectLogRecord(0,
                  RewriterApplication::NOT_APPLIED,
                  0,
                  FlushEarlyResourceInfo::CSS,
                  FlushEarlyResourceInfo::PUBLIC_CACHEABLE,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // g.js is non-rewritten JS.
  ExpectLogRecord(1,
                  RewriterApplication::NOT_APPLIED,
                  1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PUBLIC_CACHEABLE,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // h.js is rewritten JS.
  ExpectLogRecord(2,
                  RewriterApplication::APPLIED_OK,
                  2,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::PAGESPEED,
                  false /* not affected by bandwidth */,
                  true /* in HEAD */);
  // defer_javascript is not enabled.
  ExpectLogRecord(3,
                  RewriterApplication::NOT_APPLIED,
                  -1,
                  FlushEarlyResourceInfo::JS,
                  FlushEarlyResourceInfo::DEFERJS_SCRIPT,
                  false /* not affected by bandwidth */,
                  false /* not in HEAD */);
}

TEST_F(FlushEarlyContentWriterFilterTest, FlushEarlyStyleAsScript) {
  GoogleString html_input =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<link type=\"text/css\" rel=\"stylesheet\" "
      "href=\"a.css.pagespeed.cf.xxxx.css\">"
      "<style data-pagespeed-flush-style=\"123\">b_used {color: blue }"
      "</style>\n"
      "<link type=\"text/css\" rel=\"stylesheet\""
      "href=\"d.css.pagespeed.cf.xxxx.css\" data-pagespeed-size=\"1000\"/>"
      "</head>"
      "<body>"
      "<style data-pagespeed-flush-style=\"345\">c_used {color: cyan }"
      "</style>\n"
      "</body></html>";

  const char kCssLinkPrefetchTag[] =
      "<link rel=\"stylesheet\" href=\"%s*\"/>\n";

  GoogleString html_output = StrCat(
      StringPrintf(kCssLinkPrefetchTag, "a.css"),
      "<script type=\"text/psa_flush_style\" id=\"123\">"
      "b_used {color: blue }</script>",
      StringPrintf(kCssLinkPrefetchTag, "d.css"),
      "<script type=\"text/psa_flush_style\" id=\"345\">"
      "c_used {color: cyan }</script>",
      FlushEarlyContentWriterFilter::kDisableLinkTag,
      StringPrintf(kPrefetchScript, 4));

  ResetUserAgent("prefetch_image_tag");
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kPrioritizeCriticalCss);
  options()->set_enable_flush_early_critical_css(true);
  server_context()->ComputeSignature(options());

  Parse("prefetch_link_script_tag", html_input);
  EXPECT_TRUE(Wildcard(html_output).Match(output_)) <<
      "Expected:\n" << html_output << "\nGot:\n" << output_;
}

TEST_F(FlushEarlyContentWriterFilterTest, DontFlushEarlyStyleIfFlagDisabled) {
  GoogleString html_input =
      "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<style data-pagespeed-flush-style=\"123\">b_used {color: blue }"
      "</style>\n"
      "</head>"
      "<body>"
      "<style data-pagespeed-flush-style=\"345\">c_used {color: cyan }"
      "</style>\n"
      "</body></html>";

  GoogleString html_output = StringPrintf(kPrefetchScript, 0);

  ResetUserAgent("prefetch_image_tag");
  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kPrioritizeCriticalCss);
  options()->set_enable_flush_early_critical_css(false);
  server_context()->ComputeSignature(options());

  Parse("prefetch_link_script_tag", html_input);
  EXPECT_TRUE(Wildcard(html_output).Match(output_)) <<
      "Expected:\n" << html_output << "\nGot:\n" << output_;
}

}  // namespace net_instaweb
