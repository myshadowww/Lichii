// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>
#include "common/fs/path_util.h"
#include "qt_common/util/game.h"

namespace {
class MetadataDirectory {
public:
    MetadataDirectory() {
        REQUIRE(directory.isValid());
        Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir,
                               std::filesystem::path{directory.path().toStdU16String()});
    }
    ~MetadataDirectory() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, previous);
    }

private:
    const std::filesystem::path previous = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    QTemporaryDir directory;
};
} // namespace

TEST_CASE("Library names are isolated by program and failed replacements retain the old name",
          "[qt_game_metadata]") {
    const MetadataDirectory directory;
    using namespace QtCommon::Game;
    REQUIRE(SaveCustomTitle(1, "  First   game  "));
    REQUIRE(SaveCustomTitle(2, "Second game"));
    REQUIRE(ReadCustomTitle(1) == "First game");
    REQUIRE(ReadCustomTitle(2) == "Second game");
    REQUIRE_FALSE(SaveCustomTitle(1, std::string(4097, 'x')));
    REQUIRE(ReadCustomTitle(1) == "First game");
    REQUIRE_FALSE(SaveCustomTitle(0, "Unknown game"));
    REQUIRE_FALSE(std::filesystem::exists(CustomTitlePath(0)));
    REQUIRE(SaveCustomTitle(1, " \t "));
    REQUIRE(ReadCustomTitle(1).empty());
    REQUIRE(ReadCustomTitle(2) == "Second game");
    REQUIRE(SaveCustomTitle(1, ""));
}

TEST_CASE("Library artwork scales large images and invalid replacements preserve saved pixels",
          "[qt_game_metadata]") {
    const MetadataDirectory directory;
    using namespace QtCommon::Game;
    QImage image(2048, 512, QImage::Format_ARGB32);
    image.fill(Qt::red);
    REQUIRE(SaveCustomArtwork(1, image));
    const auto bytes = ReadCustomArtwork(1);
    const auto saved = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()), "PNG");
    REQUIRE(saved.size() == QSize(1024, 256));
    REQUIRE(saved.pixelColor(0, 0) == QColor(Qt::red));
    REQUIRE(ReadCustomArtwork(2).empty());
    REQUIRE_FALSE(SaveCustomArtwork(1, QImage{}));
    REQUIRE(ReadCustomArtwork(1) == bytes);
    REQUIRE_FALSE(SaveCustomArtwork(0, image));
    REQUIRE(RemoveCustomArtwork(1));
    REQUIRE(ReadCustomArtwork(1).empty());
    REQUIRE(RemoveCustomArtwork(1));
}

TEST_CASE("Unreadable library overrides are not reported as successfully replaced or removed",
          "[qt_game_metadata]") {
    const MetadataDirectory directory;
    using namespace QtCommon::Game;
    REQUIRE(SaveCustomTitle(1, "Name"));
    {
        std::ofstream file(CustomTitlePath(1), std::ios::binary | std::ios::trunc);
        file << std::string(4097, 'x');
        REQUIRE(file.good());
    }
    REQUIRE(ReadCustomTitle(1).empty());
    const auto blocked = CustomArtworkPath(2);
    REQUIRE(std::filesystem::create_directories(blocked));
    std::ofstream(blocked / "occupied") << "keep";
    QImage image(1, 1, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    REQUIRE_FALSE(SaveCustomArtwork(2, image));
    REQUIRE_FALSE(RemoveCustomArtwork(2));
    REQUIRE(std::filesystem::is_regular_file(blocked / "occupied"));
}
