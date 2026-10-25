#import "MacSearchBridge_Internal.h"
#import "MacSearchBridge+Content.h"
#include "PathUtils.h"
#include "Logger.h"
#include "HighlightHintExtractor.h"

@implementation MEFileResult

- (instancetype)initWithName:(NSString *)name
                        path:(NSString *)path
                        type:(uint8_t)type
                        size:(uint64_t)size
                     modTime:(time_t)modTime {
    self = [super init];
    if (self) {
        _name = [name copy];
        _path = [path copy];
        _type = type;
        _size = size;
        _modTime = modTime;
    }
    return self;
}

@end

@implementation MEContentResult

- (instancetype)initWithFileName:(NSString *)fileName
                        filePath:(NSString *)filePath
                         snippet:(NSString *)snippet
                     matchOffset:(uint32_t)matchOffset
                        fileType:(uint8_t)fileType {
    self = [super init];
    if (self) {
        _fileName = [fileName copy];
        _filePath = [filePath copy];
        _snippet = [snippet copy];
        _matchOffset = matchOffset;
        _fileType = fileType;
    }
    return self;
}

@end

@implementation MEHighlightHint

- (instancetype)initWithText:(NSString *)text
                       field:(uint8_t)field
                   matchMode:(uint8_t)matchMode
               caseSensitive:(BOOL)caseSensitive {
    self = [super init];
    if (self) {
        _text = [text copy];
        _field = field;
        _matchMode = matchMode;
        _caseSensitive = caseSensitive;
    }
    return self;
}

@end

@implementation MacSearchBridge

+ (instancetype)shared {
    static MacSearchBridge *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[MacSearchBridge alloc] init];
    });
    return instance;
}

+ (void)initializeLogger {
    std::string logDir = PathUtils::getDefaultLogPath();
#ifdef DEBUG
    me::Logger::instance().init(logDir, me::LogLevel::Debug);
#else
    me::Logger::instance().init(logDir, me::LogLevel::Info);
#endif
    LOG_INFO("App", "Logger initialized");
}

+ (void)logMessage:(NSString *)message level:(int)level module:(NSString *)module {
    me::LogLevel lvl;
    switch (level) {
        case 0:  lvl = me::LogLevel::Debug; break;
        case 1:  lvl = me::LogLevel::Info;  break;
        case 2:  lvl = me::LogLevel::Warn;  break;
        case 3:  lvl = me::LogLevel::Error; break;
        default: lvl = me::LogLevel::Info;  break;
    }
    me::Logger::instance().log(lvl, [module UTF8String], [message UTF8String]);
}

+ (NSString *)logFilePath {
    auto path = me::Logger::instance().getLogFilePath();
    return [NSString stringWithUTF8String:path.c_str()];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        ServiceConfig config;
        config.scanRoot = "/";
        config.scanNetworkVolumes = true;  // index SMB/WebDAV/NFS mounts under /Volumes
        config.cachePath = PathUtils::getDefaultCachePath();
        config.logPath = PathUtils::getDefaultLogPath();
        config.httpPort = 19860;
        _serviceEngine = std::make_shared<ServiceEngine>(config);

        // Pre-install admin callbacks so HttpServer has them when auto-started
        [self _installAdminCallbacks];
    }
    return self;
}

// ═══════════════════════════════════════════════════════
//  State properties — forward to ServiceEngine
// ═══════════════════════════════════════════════════════

- (BOOL)isScanning {
    return _serviceEngine->isScanning();
}

- (BOOL)isMonitoring {
    return _serviceEngine->isMonitoring();
}

- (BOOL)isSyncing {
    return _serviceEngine->isSyncing();
}

// ═══════════════════════════════════════════════════════
//  Lifecycle — forward to ServiceEngine, wrap callbacks to main queue
// ═══════════════════════════════════════════════════════

- (void)_installCallbacks {
    __weak MacSearchBridge *weakSelf = self;

    _serviceEngine->onScanProgress = [weakSelf](uint64_t scanned, uint64_t dirs) {
        MacSearchBridge *s = weakSelf;
        if (!s) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.onScanProgress) s.onScanProgress(scanned, dirs);
        });
    };
    _serviceEngine->onIndexChanged = [weakSelf]() {
        MacSearchBridge *s = weakSelf;
        if (!s) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.onIndexChanged) s.onIndexChanged();
        });
    };
    _serviceEngine->onContentIndexProgress = [weakSelf](uint64_t indexed, uint64_t total) {
        MacSearchBridge *s = weakSelf;
        if (!s) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.onContentIndexProgress) s.onContentIndexProgress((uint32_t)indexed, (uint32_t)total);
        });
    };
    _serviceEngine->onContentIndexComplete = [weakSelf](uint32_t totalIndexed) {
        MacSearchBridge *s = weakSelf;
        if (!s) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.onContentIndexComplete) s.onContentIndexComplete(totalIndexed);
        });
    };
}

- (void)startScanFrom:(NSString *)rootPath
           completion:(void (^)(uint32_t totalRecords))completion {
    [self _installCallbacks];

    _serviceEngine->startFullScan([completion](uint32_t count, bool) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) completion(count);
        });
    });
}

- (void)startIncrementalFrom:(NSString *)rootPath
                   cachePath:(NSString *)cachePath
                     walPath:(NSString *)walPath
                  completion:(void (^)(uint32_t totalRecords, BOOL didFullScan))completion {
    [self _installCallbacks];

    _serviceEngine->startIncremental([completion](uint32_t count, bool didFullScan) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) completion(count, didFullScan ? YES : NO);
        });
    });
}

- (void)compactIndex {
    _serviceEngine->compactIndex();
}

- (void)_installAdminCallbacks {
    HttpServer::AdminCallbacks callbacks;

    callbacks.onRebuildIndex = [] {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"rebuildIndex" object:nil];
        });
    };

    __weak MacSearchBridge *weakSelf = self;
    callbacks.onRebuildContentIndex = [weakSelf] {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            strongSelf->_serviceEngine->rebuildContentIndex();
        });
    };

    callbacks.onSetContentConfig = [weakSelf](const std::vector<std::string>& exts, uint64_t maxSize) {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        auto ci = strongSelf->_serviceEngine->safeContentIndex();
        if (!ci) return;
        ci->setExtensions(exts);
        ci->setMaxFileSize(maxSize);
    };

    callbacks.onGetContentExtensions = [weakSelf]() -> std::vector<std::string> {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return {};
        auto ci = strongSelf->_serviceEngine->safeContentIndex();
        if (!ci) return {};
        return ci->getExtensions();
    };

    callbacks.onGetContentMaxFileSize = [weakSelf]() -> uint64_t {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return 0;
        auto ci = strongSelf->_serviceEngine->safeContentIndex();
        if (!ci) return 0;
        return ci->getMaxFileSize();
    };

    _serviceEngine->adminCallbacks = std::move(callbacks);
}

- (void)startHttpServer:(uint16_t)port {
    [self _installAdminCallbacks];
    _serviceEngine->startHttpServer(port);
}

- (void)stopHttpServer {
    _serviceEngine->stopHttpServer();
}

- (void)prepareForTermination {
    _serviceEngine->shutdown();
    LOG_INFO("Logger", "=== Log session ended ===");
    me::Logger::instance().shutdown();
}

- (void)stopMonitoring {
    // ServiceEngine's stopMonitoring is private; it's called internally during shutdown.
    // For external callers (e.g., before a manual rescan), we don't need this.
    // The Bridge's public API includes stopMonitoring but it's not commonly used externally.
    // For compatibility, this is a no-op — monitoring is managed by ServiceEngine lifecycle.
}

- (void)rescanSubtree:(NSString *)dirPath {
    _serviceEngine->rescanSubtree(std::string([dirPath UTF8String]));
}

// ═══════════════════════════════════════════════════════
//  Query methods — type conversion layer
// ═══════════════════════════════════════════════════════

- (NSArray<NSNumber *> *)queryIndices:(NSString *)keyword
                           maxResults:(uint32_t)maxResults {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = engine->query(key, maxResults);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

- (NSArray<MEFileResult *> *)recordsAtIndices:(NSArray<NSNumber *> *)indices {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    std::vector<uint32_t> idxVec;
    idxVec.reserve(indices.count);
    for (NSNumber *num in indices) {
        idxVec.push_back([num unsignedIntValue]);
    }

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.count];
    engine->forEachRecordWithPath(idxVec, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return;
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (NSArray<NSNumber *> *)recentIndices:(uint32_t)count {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    auto indices = engine->recentIndices(count);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

- (NSArray<MEFileResult *> *)queryResults:(NSString *)keyword
                               maxResults:(uint32_t)maxResults {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = engine->query(key, maxResults);
    if (indices.empty()) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.size()];
    engine->forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return;
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (NSArray<MEFileResult *> *)queryResults:(NSString *)keyword
                               maxResults:(uint32_t)maxResults
                                sessionId:(uint64_t)sessionId {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = engine->query(key, maxResults, true, sessionId);
    if (indices.empty()) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.size()];
    engine->forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return;
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (void)cancelSession:(uint64_t)sessionId {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return;
    engine->cancelSession(sessionId);
}

- (NSArray<MEFileResult *> *)recentResults:(uint32_t)count {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return @[];

    auto indices = engine->recentIndices(count);
    if (indices.empty()) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.size()];
    engine->forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return;
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (NSArray<MEHighlightHint *> *)parseHighlightHints:(NSString *)query {
    std::string q([query UTF8String]);
    auto hints = extractHighlightHints(q);

    NSMutableArray<MEHighlightHint *> *result = [NSMutableArray arrayWithCapacity:hints.size()];
    for (auto& h : hints) {
        NSString *text = [NSString stringWithUTF8String:h.text.c_str()];
        if (!text) continue;
        [result addObject:[[MEHighlightHint alloc] initWithText:text
                                                         field:static_cast<uint8_t>(h.field)
                                                     matchMode:static_cast<uint8_t>(h.mode)
                                                 caseSensitive:h.caseSensitive ? YES : NO]];
    }
    return result;
}

- (uint32_t)recordCount {
    return _serviceEngine->recordCount();
}

- (uint32_t)liveRecordCount {
    return _serviceEngine->liveRecordCount();
}

- (BOOL)saveIndexToFile:(NSString *)path {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return NO;
    IndexMetadata meta;
    meta.lastEventId = 0;
    meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
    meta.extra[IndexMetadata::kOSVersion] = PathUtils::getOSVersionString();
    meta.extra[IndexMetadata::kRecordFormat] = "v3_inode";
    return engine->saveToFile(std::string([path UTF8String]), meta) ? YES : NO;
}

- (BOOL)loadIndexFromFile:(NSString *)path {
    auto engine = _serviceEngine->safeEngine();
    if (!engine) return NO;
    return engine->loadFromFile(std::string([path UTF8String])) ? YES : NO;
}

@end
