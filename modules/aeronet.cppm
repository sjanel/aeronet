// aeronet Module
//
// Import this module as a modular form of <aeronet/aeronet.hpp>.

module;

#include <aeronet/aeronet.hpp>

export module aeronet;

export namespace aeronet {
    using aeronet::SingleHttpServer;
    using aeronet::HttpRequest;
    using aeronet::HttpResponse;
    using aeronet::HttpServerConfig;
    using aeronet::HttpResponseWriter;
    using aeronet::MultiHttpServer;
    using aeronet::RequestMiddleware;
    using aeronet::ResponseMiddleware;
    using aeronet::MultipartFormDataOptions;
    using aeronet::MultipartFormData;
    using aeronet::RequestHandler;
    using aeronet::AsyncRequestHandler;
    using aeronet::StreamingHandler;
    using aeronet::RouterConfig;
    using aeronet::Router;
    using aeronet::StaticFileHandler;
    using aeronet::EncodingSelector;
    using aeronet::BrotliEncoderContext;
    using aeronet::BrotliEncoder;
    using aeronet::BuiltinProbesConfig;
    using aeronet::CompressionConfig;
    using aeronet::ConcatenatedHeaderValues;
    using aeronet::ConcatenatedHeaders;
    using aeronet::DecompressionConfig;
    using aeronet::DogStatsD;
    using aeronet::EncoderContext;
    using aeronet::Encoding;
    using aeronet::GetEncodingStr;
    using aeronet::IsEncodingEnabled;
    using aeronet::FilePayload;
    using aeronet::HeadersViewMap;
    using aeronet::HttpPayload;
    using aeronet::HttpResponseData;
    using aeronet::HttpServerConfig;
    using aeronet::Http2Config;
    using aeronet::ProtocolType;
    using aeronet::ProtocolProcessResult;
    using aeronet::IProtocolHandler;
    using aeronet::ProtocolHandlerFactory;
    using aeronet::RequestTask;
    using aeronet::ServerStats;
    using aeronet::StaticFileConfig;
    using aeronet::TrimOws;
    using aeronet::TelemetryConfig;
    using aeronet::TLSConfig;
    using aeronet::TLSInfo;
    using aeronet::ZlibEncoderContext;
    using aeronet::ZlibEncoder;
    using aeronet::ZStreamRAII;
    using aeronet::ZstdEncoderContext;
    using aeronet::ZstdEncoder;
    using aeronet::BaseFd;
    using aeronet::Connection;
    using aeronet::EventFd;
    using aeronet::EventLoop;
    using aeronet::EventBmp;
    using aeronet::File;
    using aeronet::Socket;
    using aeronet::TimerFd;
    using aeronet::TransportHint;
    using aeronet::ITransport;
    using aeronet::PlainTransport;
    using aeronet::ConcatenatedStrings;
    using aeronet::ConcatenatedStrings32;
    using aeronet::DynamicConcatenatedStrings;
    #ifdef AERONET_ENABLE_OPENSSL
    using aeronet::TlsContext;
    using aeronet::TlsTicketKeyStore;
    using aeronet::TlsHandshakeEvent;
    using aeronet::TlsHandshakeCallback;
    using aeronet::TlsHandshakeObserver;
    using aeronet::SetTlsHandshakeObserver;
    using aeronet::GetTlsHandshakeObserver;
    using aeronet::KtlsEnableResult;
    using aeronet::TlsMetricsInternal;
    using aeronet::TlsTicketKeyStore;
    using aeronet::TlsTransport;
    #endif
    using aeronet::MajorMinorVersion;
    using aeronet::MIMEMapping;
    using aeronet::MIMETypeIdx;
    using aeronet::DetermineMIMETypeIdx;
    using aeronet::DetermineMIMETypeStr;
    using aeronet::ObjectArrayPool;
    using aeronet::ObjectPool;
    using aeronet::RawBytes;
    using aeronet::RawBytes32;
    using aeronet::RawChars;
    using aeronet::RawChars32;
    using aeronet::SafeCast;
    using aeronet::SignalHandler;
    using aeronet::StaticConcatenatedStrings;
    using aeronet::JoinStringView;
    using aeronet::JoinStringView_v;
    using aeronet::JoinStringViewWithSep;
    using aeronet::JoinStringViewWithSep_v;
    using aeronet::IntToStringView;
    using aeronet::IntToStringView_v;
    using aeronet::CharToStringView;
    using aeronet::CharToStringView_v;
    using aeronet::CaseInsensitiveEqual;
    using aeronet::CaseInsensitiveLess;
    using aeronet::StartsWithCaseInsensitive;
    using aeronet::CaseInsensitiveHashFunc;
    using aeronet::CaseInsensitiveEqualFunc;
    using aeronet::IntegralToCharVector;
    using aeronet::StringToIntegral;
    using aeronet::AppendIntegralToCharBuf;
    using aeronet::IntegralToCharBuffer;
    using aeronet::SysClock;
    using aeronet::SysTimePoint;
    using aeronet::SysDuration;
    using aeronet::SteadyClock;
    #ifdef AERONET_ENABLE_WEBSOCKET
    using aeronet::WebSocketHandlerFactory;
    using aeronet::WebSocketEndpoint;
    #endif

    using aeronet::kNbContentEncodings;
    using aeronet::EventIn;
    using aeronet::EventOut;
    using aeronet::EventErr;
    using aeronet::EventHup;
    using aeronet::EventRdHup;
    using aeronet::EventEt;
    using aeronet::Connection;
    using aeronet::kUnknownMIMEMappingIdx;
    using aeronet::kMIMEMappings;

    using aeronet::version;
    using aeronet::fullVersionStringView;
    using aeronet::spdLogEnabled;
    using aeronet::openSslEnabled;
    using aeronet::http2Enabled;
    using aeronet::zlibEnabled;
    using aeronet::zstdEnabled;
    using aeronet::brotliEnabled;
    using aeronet::openTelemetryEnabled;
    using aeronet::webSocketEnabled;
    using aeronet::flat_hash_map;
    using aeronet::nchars;
    using aeronet::ndigits;
    using aeronet::write2;
    using aeronet::write3;
    using aeronet::write4;
    using aeronet::copy3;
    using aeronet::read2;
    using aeronet::read3;
    using aeronet::read4;
    using aeronet::read6;
    using aeronet::read9;
    using aeronet::make_joined_string_view;
    using aeronet::tolower;
    using aeronet::toupper;

    namespace http {
        using aeronet::http::MethodBmp;
        using aeronet::http::Method;
        using aeronet::http::MethodIdx;
        using aeronet::http::IsMethodSet;
        using aeronet::http::IsMethodIdxSet;
        using aeronet::http::MethodToIdx;
        using aeronet::http::MethodFromIdx;
        using aeronet::http::MethodBmpFromIdx;
        using aeronet::http::MethodIdxToStr;
        using aeronet::http::MethodToStr;
        using aeronet::http::Version;
        
        using aeronet::http::kNbMethods;
        using aeronet::http::kMethodStrings;
        using aeronet::http::kAllMethodsStrLen;
        using aeronet::http::kHttpPrefix;
        using aeronet::http::HTTP_1_0;
        using aeronet::http::HTTP_1_1;
        using aeronet::http::HTTP_2_0;
    
        using aeronet::http::operator|;
    }

    namespace http2 {
        #ifdef AERONET_ENABLE_HTTP2
        using aeronet::http2::Http2ProtocolHandler;
        using aeronet::http2::Http2Stream;
        using aeronet::http2::GetHpackStaticTable;
        using aeronet::http2::HpackDynamicEntry;
        using aeronet::http2::HpackDynamicTable;
        using aeronet::http2::HpackLookupResult;
        using aeronet::http2::HpackDecoder;
        using aeronet::http2::HpackEncoder;
        using aeronet::http2::ConnectionState;
        using aeronet::http2::DataCallback;
        using aeronet::http2::StreamEventCallback;
        using aeronet::http2::Http2Connection;
        using aeronet::http2::FrameTypeName;
        using aeronet::http2::ErrorCodeName;
        using aeronet::http2::FrameHeader;
        using aeronet::http2::ParseFrameHeader;
        using aeronet::http2::WriteFrameHeader;
        using aeronet::http2::WriteFrame;
        using aeronet::http2::SettingsEntry;
        using aeronet::http2::DataFrame;
        using aeronet::http2::HeadersFrame;
        using aeronet::http2::PriorityFrame;
        using aeronet::http2::RstStreamFrame;
        using aeronet::http2::SettingsFrame;
        using aeronet::http2::PingFrame;
        using aeronet::http2::GoAwayFrame;
        using aeronet::http2::WindowUpdateFrame;
        using aeronet::http2::ContinuationFrame;
        using aeronet::http2::FrameParseResult;
        using aeronet::http2::ParseDataFrame;
        using aeronet::http2::ParseHeadersFrame;
        using aeronet::http2::ParsePriorityFrame;
        using aeronet::http2::ParseRstStreamFrame;
        using aeronet::http2::ParseSettingsFrame;
        using aeronet::http2::ParsePingFrame;
        using aeronet::http2::ParseGoAwayFrame;
        using aeronet::http2::ParseWindowUpdateFrame;
        using aeronet::http2::ParseContinuationFrame;
        using aeronet::http2::ComputeHeaderFrameFlags;
        using aeronet::http2::WriteDataFrame;
        using aeronet::http2::WriteHeadersFrameWithPriority;
        using aeronet::http2::WritePriorityFrame;
        using aeronet::http2::WriteRstStreamFrame;
        using aeronet::http2::WriteSettingsFrame;
        using aeronet::http2::WriteSettingsAckFrame;
        using aeronet::http2::WritePingFrame;
        using aeronet::http2::WriteGoAwayFrame;
        using aeronet::http2::WriteWindowUpdateFrame;
        using aeronet::http2::WriteContinuationFrame;
        using aeronet::http2::CreateHttp2ProtocolHandler;
        using aeronet::http2::StreamStateName;
        using aeronet::http2::FrameType;
        using aeronet::http2::ErrorCode;
        using aeronet::http2::SettingsParameter;
        using aeronet::http2::IsClientStream;
        using aeronet::http2::IsServerStream;
        using aeronet::http2::StreamState;

        namespace FrameFlags {
            using aeronet::http2::FrameFlags::DataEndStream;
            using aeronet::http2::FrameFlags::DataPadded;
            using aeronet::http2::FrameFlags::HeadersEndStream;
            using aeronet::http2::FrameFlags::HeadersEndHeaders;
            using aeronet::http2::FrameFlags::HeadersPadded;
            using aeronet::http2::FrameFlags::HeadersPriority;
            using aeronet::http2::FrameFlags::SettingsAck;
            using aeronet::http2::FrameFlags::PingAck;
            using aeronet::http2::FrameFlags::ContinuationEndHeaders;
        }
        
        using aeronet::http2::kConnectionPreface;
        using aeronet::http2::kAlpnH2;
        using aeronet::http2::kAlpnH2c;
        using aeronet::http2::kDefaultHeaderTableSize;
        using aeronet::http2::kDefaultEnablePush;
        using aeronet::http2::kDefaultMaxConcurrentStreams;
        using aeronet::http2::kDefaultInitialWindowSize;
        using aeronet::http2::kDefaultMaxFrameSize;
        using aeronet::http2::kDefaultMaxHeaderListSize;
        using aeronet::http2::kMinMaxFrameSize;
        using aeronet::http2::kMaxMaxFrameSize;
        using aeronet::http2::kMaxWindowSize;
        using aeronet::http2::kMaxStreamId;
        using aeronet::http2::kFrameHeaderSize;
        using aeronet::http2::kConnectionStreamId;
        #endif
    }

    namespace tracing {
        using aeronet::tracing::Span;
        using aeronet::tracing::SpanPtr;
        using aeronet::tracing::SpanRAII;
        using aeronet::tracing::TelemetryContextImpl;
        using aeronet::tracing::TelemetryContext;
    }

    namespace websocket {
        #ifdef AERONET_ENABLE_WEBSOCKET
        using aeronet::websocket::IsControlFrame;
        using aeronet::websocket::IsDataFrame;
        using aeronet::websocket::IsReservedOpcode;
        using aeronet::websocket::CloseCode;
        using aeronet::websocket::IsValidWireCloseCode;
        using aeronet::websocket::DeflateConfig;
        using aeronet::websocket::DeflateNegotiatedParams;
        using aeronet::websocket::ParseDeflateOffer;
        using aeronet::websocket::BuildDeflateResponse;
        using aeronet::websocket::DeflateContext;
        using aeronet::websocket::MaskingKey;
        using aeronet::websocket::FrameHeader;
        using aeronet::websocket::FrameParseResult;
        using aeronet::websocket::ParseFrame;
        using aeronet::websocket::ApplyMask;
        using aeronet::websocket::BuildFrame;
        using aeronet::websocket::BuildCloseFrame;
        using aeronet::websocket::ClosePayload;
        using aeronet::websocket::ParseClosePayload;
        using aeronet::websocket::WebSocketConfig;
        using aeronet::websocket::WebSocketCallbacks;
        using aeronet::websocket::WebSocketHandler;
        using aeronet::websocket::CreateServerWebSocketHandler;
        using aeronet::websocket::CreateClientWebSocketHandler;

        using aeronet::websocket::kGUID;
        using aeronet::websocket::kWebSocketVersion;
        using aeronet::websocket::SecWebSocketKey;
        using aeronet::websocket::SecWebSocketAccept;
        using aeronet::websocket::SecWebSocketProtocol;
        using aeronet::websocket::UpgradeValue;
        using aeronet::websocket::kFinBit;
        using aeronet::websocket::kRsv1Bit;
        using aeronet::websocket::kRsv2Bit;
        using aeronet::websocket::kRsv3Bit;
        using aeronet::websocket::kOpcodeMask;
        using aeronet::websocket::kMaskBit;
        using aeronet::websocket::kPayloadLenMask;
        using aeronet::websocket::kPayloadLen16;
        using aeronet::websocket::kPayloadLen64;
        using aeronet::websocket::kMaxControlFramePayload;
        using aeronet::websocket::kMaskingKeySize;
        using aeronet::websocket::kMinFrameHeaderSize;
        using aeronet::websocket::kMaxFrameHeaderSize;
        using aeronet::websocket::kMaxClientFrameHeaderSize;
        using aeronet::websocket::kMaxServerFrameHeaderSize;
        using aeronet::websocket::kDefaultMaxMessageSize;
        using aeronet::websocket::kDefaultMaxFrameSize;
        #endif
    }
}
