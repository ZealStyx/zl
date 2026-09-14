// Git dependency URL allowlist: helper transports and unauthenticated
// schemes must never reach `git clone`.
#include "git.hpp"

#include <iostream>
#include <string>
#include <vector>

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "zlpkg-git-url: " << message << '\n';
    ++failures;
}

void expectAllowed(const std::string& url) {
    require(zlpkg::isAllowedGitDependencyUrl(url), "should allow '" + url + "'");
}

void expectRejected(const std::string& url) {
    require(!zlpkg::isAllowedGitDependencyUrl(url), "should reject '" + url + "'");
}

int main() {
    expectAllowed("https://example.com/zl-ecs.git");
    expectAllowed("https://github.com/org/repo.git");
    expectAllowed("ssh://git@example.com/org/repo.git");
    expectAllowed("git@github.com:org/repo.git");

    expectRejected("");
    expectRejected("-https://example.com/repo.git");
    expectRejected("/etc/passwd");
    expectRejected("http://example.com/repo.git");
    expectRejected("git://example.com/repo.git");
    expectRejected("file:///tmp/repo.git");
    expectRejected("ext::sh -c 'id'");
    expectRejected("ext::popen cat /etc/passwd");
    expectRejected("http::https://example.com/repo.git");
    expectRejected("../relative/path");
    expectRejected("repo.git");

    if (failures != 0) {
        std::cerr << "zlpkg-git-url: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
