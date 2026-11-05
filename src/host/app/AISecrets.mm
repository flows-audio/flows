#include "JuceServices.hpp"
#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace flows::app {
namespace {
NSMutableDictionary* query(const std::string& credentialId) {
    return [@{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService : @"audio.flows.ai-models",
              (__bridge id)kSecAttrAccount : [NSString stringWithUTF8String:credentialId.c_str()]} mutableCopy];
}
void checkStatus(OSStatus status) {
    if (status != errSecSuccess)
        throw std::runtime_error("macOS Keychain could not access the API key (status " + std::to_string(status) +
                                 "). Unlock Keychain and try again.");
}
}  // namespace
AISecrets systemAISecrets() {
    AISecrets secrets;
    secrets.read = [](const std::string& credentialId) -> std::string {
        @autoreleasepool {
            auto q = query(credentialId);
            q[(__bridge id)kSecReturnData] = @YES;
            q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
            CFTypeRef result = nullptr;
            const auto status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &result);
            [q release];
            if (status == errSecItemNotFound) return {};
            checkStatus(status);
            auto bytes = (CFDataRef)result;
            std::string key(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), static_cast<std::size_t>(CFDataGetLength(bytes)));
            CFRelease(result);
            return key;
        }
    };
    secrets.write = [](const std::string& credentialId, const std::string& key) {
        @autoreleasepool {
            auto q = query(credentialId);
            q[(__bridge id)kSecValueData] = [NSData dataWithBytes:key.data() length:key.size()];
            q[(__bridge id)kSecAttrLabel] = @"Flows AI model";
            const auto status = SecItemAdd((__bridge CFDictionaryRef)q, nullptr);
            [q release];
            checkStatus(status);
        }
    };
    secrets.remove = [](const std::string& credentialId) {
        @autoreleasepool {
            auto q = query(credentialId);
            const auto status = SecItemDelete((__bridge CFDictionaryRef)q);
            [q release];
            if (status != errSecItemNotFound) checkStatus(status);
        }
    };
    secrets.persistent = true;
    return secrets;
}
}  // namespace flows::app
