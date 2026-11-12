/**
 * Texture compression on the GPU: each format the viewer chooses, from synthetic images
 * like the ones it gets, decoded back by the hardware and compared with the source; then
 * how fast a batch of large textures encodes. See
 * doc/ayanestorm-new-renderer-texture-compression.md.
 * Author: chanayane@firestorm
 */
#include "asrtestvram.h"
#include "asrtestwindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace asrtest
{
namespace
{
constexpr uint32_t SIZE = 256;

struct EncodeCase
{
    const char* name;
    uint32_t channels;
    asr::TextureFormat expected;
    double minPsnr; // over the channels the format keeps
    std::vector<uint8_t> pixels; // channels bytes a texel
};

const char* formatName(asr::TextureFormat format)
{
    switch (format)
    {
        case asr::TextureFormat::BC1: return "BC1";
        case asr::TextureFormat::BC4: return "BC4";
        case asr::TextureFormat::BC5: return "BC5";
        case asr::TextureFormat::BC7: return "BC7";
        default:                      return "uncompressed";
    }
}

std::vector<EncodeCase> makeCases()
{
    std::vector<EncodeCase> cases;
    std::mt19937 random(7);
    const auto image = [](uint32_t channels) { return std::vector<uint8_t>(size_t(SIZE) * SIZE * channels); };

    // Skin or sky: a gentle gradient, where BC1 bands.
    EncodeCase gradient{ "smooth gradient", 3, asr::TextureFormat::BC7, 42.0, image(3) };
    for (uint32_t y = 0; y < SIZE; ++y)
        for (uint32_t x = 0; x < SIZE; ++x)
        {
            uint8_t* p = &gradient.pixels[(y * SIZE + x) * 3];
            p[0] = uint8_t(150 + x * 60 / SIZE);
            p[1] = uint8_t(100 + y * 50 / SIZE);
            p[2] = uint8_t(90 + (x + y) * 30 / (2 * SIZE));
        }
    cases.push_back(std::move(gradient));

    // Bricks, wood, fabric: busy detail, where BC1's error does not show.
    // Grain of +-40 in a block leaves BC1's four levels about 27 apart: near 30 dB at best.
    EncodeCase detail{ "busy detail", 3, asr::TextureFormat::BC1, 28.0, image(3) };
    std::uniform_int_distribution<int> noise(-40, 40);
    for (uint32_t y = 0; y < SIZE; ++y)
        for (uint32_t x = 0; x < SIZE; ++x)
        {
            uint8_t* p = &detail.pixels[(y * SIZE + x) * 3];
            // Grain changes brightness more than hue, as in photographed materials.
            const bool mortar = (y % 16) < 2 || ((x + (y / 16 % 2) * 16) % 32) < 2;
            const int base = mortar ? 200 : 120;
            const int grain = noise(random);
            const int tint = noise(random) / 8;
            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t(std::clamp(base - c * 30 + grain + (c == 0 ? tint : -tint), 0, 255));
        }
    cases.push_back(std::move(detail));

    // A leaf or lace: colour with alpha, hard and soft edges.
    EncodeCase alpha{ "alpha edges", 4, asr::TextureFormat::BC7, 36.0, image(4) };
    for (uint32_t y = 0; y < SIZE; ++y)
        for (uint32_t x = 0; x < SIZE; ++x)
        {
            uint8_t* p = &alpha.pixels[(y * SIZE + x) * 4];
            const float dx = float(x) - SIZE / 2.f, dy = float(y) - SIZE / 2.f;
            const float r = std::sqrt(dx * dx + dy * dy);
            p[0] = uint8_t(40 + x * 150 / SIZE);
            p[1] = uint8_t(160 - y * 100 / SIZE);
            p[2] = 60;
            p[3] = uint8_t(x < SIZE / 2 ? (r < 90.f ? 255 : 0) : std::clamp(int(255.f * (110.f - r) / 40.f), 0, 255));
        }
    cases.push_back(std::move(alpha));

    // A mask: one channel.
    EncodeCase mask{ "single channel", 1, asr::TextureFormat::BC4, 40.0, image(1) };
    for (uint32_t y = 0; y < SIZE; ++y)
        for (uint32_t x = 0; x < SIZE; ++x)
        {
            const float dx = float(x) - SIZE / 2.f, dy = float(y) - SIZE / 2.f;
            mask.pixels[y * SIZE + x] = uint8_t(std::clamp(int(255.f - std::sqrt(dx * dx + dy * dy) * 2.f), 0, 255));
        }
    cases.push_back(std::move(mask));

    // Two channels, as a normal map's X and Y.
    EncodeCase normal{ "two channels", 2, asr::TextureFormat::BC5, 40.0, image(2) };
    for (uint32_t y = 0; y < SIZE; ++y)
        for (uint32_t x = 0; x < SIZE; ++x)
        {
            uint8_t* p = &normal.pixels[(y * SIZE + x) * 2];
            p[0] = uint8_t(128 + 100 * std::sin(x * 0.1f));
            p[1] = uint8_t(128 + 100 * std::cos(y * 0.07f));
        }
    cases.push_back(std::move(normal));
    return cases;
}

std::vector<uint8_t> toRgba(const std::vector<uint8_t>& pixels, uint32_t channels)
{
    std::vector<uint8_t> rgba(size_t(SIZE) * SIZE * 4, 0);
    for (size_t i = 0; i < size_t(SIZE) * SIZE; ++i)
    {
        for (uint32_t c = 0; c < channels; ++c) rgba[i * 4 + c] = pixels[i * channels + c];
        if (channels < 4) rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

// CPU decoders, from the format specification, written apart from the encoders: on
// Windows they are checked against the hardware, then they judge the Mac's blocks.
uint32_t bits(const uint8_t* block, uint32_t& pos, uint32_t count)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; ++i, ++pos)
        value |= uint32_t((block[pos >> 3] >> (pos & 7)) & 1) << i;
    return value;
}

void decodeBC1(const uint8_t* b, uint8_t out[16][4])
{
    const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    int p[4][4];
    const auto expand = [](uint32_t c, int* rgb) {
        const uint32_t r = c >> 11 & 31, g = c >> 5 & 63, bl = c & 31;
        rgb[0] = int(r << 3 | r >> 2); rgb[1] = int(g << 2 | g >> 4); rgb[2] = int(bl << 3 | bl >> 2); rgb[3] = 255;
    };
    expand(c0, p[0]);
    expand(c1, p[1]);
    for (int c = 0; c < 3; ++c)
    {
        p[2][c] = c0 > c1 ? (2 * p[0][c] + p[1][c]) / 3 : (p[0][c] + p[1][c]) / 2;
        p[3][c] = c0 > c1 ? (p[0][c] + 2 * p[1][c]) / 3 : 0;
    }
    p[2][3] = 255;
    p[3][3] = c0 > c1 ? 255 : 0;
    const uint32_t indices = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) out[i][c] = uint8_t(p[indices >> (2 * i) & 3][c]);
}

void decodeBC4(const uint8_t* b, uint8_t out[16][4], int channel)
{
    const int e0 = b[0], e1 = b[1];
    int p[8] = { e0, e1 };
    for (int k = 2; k < 8; ++k)
        p[k] = e0 > e1 ? ((8 - k) * e0 + (k - 1) * e1) / 7 : k < 6 ? ((6 - k) * e0 + (k - 1) * e1) / 5 : k == 6 ? 0 : 255;
    uint32_t pos = 16;
    for (int i = 0; i < 16; ++i) out[i][channel] = uint8_t(p[bits(b, pos, 3)]);
}

// BC7 modes 1, 5 and 6, the ones the encoder writes; any other mode decodes magenta.
bool decodeBC7(const uint8_t* b, uint8_t out[16][4])
{
    static const uint16_t PARTITIONS[64] = {
        0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
        0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
        0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
        0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22 };
    static const uint8_t ANCHORS[64] = { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
                                         15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
    static const int W2[4] = { 0, 21, 43, 64 }, W3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 }, W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
    const auto lerp = [](int a, int c, int w) { return uint8_t(((64 - w) * a + w * c + 32) >> 6); };
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) ++mode;
    uint32_t pos = mode + 1;
    if (mode == 6)
    {
        int e[2][4];
        for (int c = 0; c < 4; ++c) { e[0][c] = int(bits(b, pos, 7)); e[1][c] = int(bits(b, pos, 7)); }
        const int p0 = int(bits(b, pos, 1)), p1 = int(bits(b, pos, 1));
        for (int c = 0; c < 4; ++c) { e[0][c] = e[0][c] << 1 | p0; e[1][c] = e[1][c] << 1 | p1; }
        for (int i = 0; i < 16; ++i)
        {
            const int w = W4[bits(b, pos, i == 0 ? 3 : 4)];
            for (int c = 0; c < 4; ++c) out[i][c] = lerp(e[0][c], e[1][c], w);
        }
        return true;
    }
    if (mode == 5)
    {
        const uint32_t rotation = bits(b, pos, 2);
        int e[2][4];
        for (int c = 0; c < 3; ++c)
        {
            for (int k = 0; k < 2; ++k)
            {
                const int v = int(bits(b, pos, 7));
                e[k][c] = v << 1 | v >> 6;
            }
        }
        e[0][3] = int(bits(b, pos, 8));
        e[1][3] = int(bits(b, pos, 8));
        int colour[16], alpha[16];
        for (int i = 0; i < 16; ++i) colour[i] = W2[bits(b, pos, i == 0 ? 1 : 2)];
        for (int i = 0; i < 16; ++i) alpha[i] = W2[bits(b, pos, i == 0 ? 1 : 2)];
        for (int i = 0; i < 16; ++i)
        {
            for (int c = 0; c < 3; ++c) out[i][c] = lerp(e[0][c], e[1][c], colour[i]);
            out[i][3] = lerp(e[0][3], e[1][3], alpha[i]);
            if (rotation) std::swap(out[i][3], out[i][rotation - 1]);
        }
        return true;
    }
    if (mode == 1)
    {
        const uint32_t shape = bits(b, pos, 6);
        int e[4][3];
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 4; ++k) e[k][c] = int(bits(b, pos, 6));
        const int p[2] = { int(bits(b, pos, 1)), int(bits(b, pos, 1)) };
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 3; ++c)
            {
                const int v = e[k][c] << 1 | p[k / 2];
                e[k][c] = v << 1 | v >> 6;
            }
        for (int i = 0; i < 16; ++i)
        {
            const int s = PARTITIONS[shape] >> i & 1;
            const int w = W3[bits(b, pos, i == 0 || i == ANCHORS[shape] ? 2 : 3)];
            for (int c = 0; c < 3; ++c) out[i][c] = lerp(e[2 * s][c], e[2 * s + 1][c], w);
            out[i][3] = 255;
        }
        return true;
    }
    for (int i = 0; i < 16; ++i) { out[i][0] = 255; out[i][1] = 0; out[i][2] = 255; out[i][3] = 255; }
    return false;
}

// A mip's blocks to RGBA8; channels a format does not keep read 0, alpha 255.
std::vector<uint8_t> decodeBlocks(asr::TextureFormat format, const std::vector<uint8_t>& data, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    const uint32_t blockBytes = asr::textureBlockBytes(format);
    const uint32_t across = (width + 3) / 4;
    for (uint32_t by = 0; by < (height + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < across; ++bx)
        {
            const uint8_t* block = &data[(size_t(by) * across + bx) * blockBytes];
            uint8_t out[16][4] = {};
            for (auto& texel : out) texel[3] = 255;
            if (format == asr::TextureFormat::BC1) decodeBC1(block, out);
            else if (format == asr::TextureFormat::BC4) decodeBC4(block, out, 0);
            else if (format == asr::TextureFormat::BC5) { decodeBC4(block, out, 0); decodeBC4(block + 8, out, 1); }
            else decodeBC7(block, out);
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < width && y < height) std::copy(out[i], out[i] + 4, &rgba[(size_t(y) * width + x) * 4]);
            }
        }
    return rgba;
}

// Peak signal to noise over the channels the format keeps, in dB.
double psnr(const std::vector<uint8_t>& source, const std::vector<uint8_t>& decoded, uint32_t texels, uint32_t channels)
{
    double sum = 0.0;
    for (size_t i = 0; i < texels; ++i)
        for (uint32_t c = 0; c < channels; ++c)
        {
            const double d = double(source[i * 4 + c]) - double(decoded[i * 4 + c]);
            sum += d * d;
        }
    const double mse = sum / (double(texels) * channels);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
}

int runEncodeTest(const asr::DeviceOptions& options)
{
    using Clock = std::chrono::steady_clock;
    TestWindow window;
    if (!window.create(320, 240, "asrenderer_test: texture encoding")) return 1;
    std::string error;
    std::unique_ptr<asr::Device> device = asr::Device::create(window.surfaceTarget(), options, error);
    if (!device)
    {
        printf("FAIL device: %s\n", error.c_str());
        return 1;
    }
    asr::TextureQueue& textures = device->textures();
    if (!textures.compressionSupported())
    {
        printf("SKIP %s does not sample BC formats\n", device->info().name.c_str());
        return 0;
    }
    asr::UIList ui;
    ui.setSize(320, 240);
    ui.generation = 1;
    bool okay = true;
    bool rendering = true;
    const auto frames = [&](uint32_t count) {
        for (uint32_t i = 0; i < count && rendering; ++i)
        {
            textures.flush();
            asr::FrameInput input;
            input.width = 320;
            input.height = 240;
            input.ui = &ui;
            if (!window.pumpEvents() || !device->renderFrame(input))
            {
                printf("FAIL render: %s\n", device->lastError().c_str());
                rendering = false;
                okay = false;
            }
        }
        return rendering;
    };
    const auto settle = [&]() {
        for (int i = 0; i < 2000 && rendering; ++i)
        {
            frames(1);
            if (device->frameParts().textureCommandsLeft == 0 && textures.waitingBytes() == 0) return true;
        }
        return false;
    };
    frames(3);

    constexpr uint32_t MIPS = 9; // 256 down to 1
    std::vector<EncodeCase> cases = makeCases();
    std::vector<uint32_t> slots;
    for (EncodeCase& c : cases)
    {
        const asr::TextureFormat chosen = asr::chooseTextureFormat(c.pixels.data(), SIZE, SIZE, c.channels);
        if (chosen != c.expected)
        {
            printf("FAIL %s: chose %s, expected %s\n", c.name, formatName(chosen), formatName(c.expected));
            okay = false;
        }
        asr::TextureDesc desc;
        desc.width = SIZE;
        desc.height = SIZE;
        desc.mipLevels = MIPS;
        desc.format = c.expected;
        const std::vector<uint8_t> rgba = toRgba(c.pixels, c.channels);
        const uint32_t slot = textures.create(desc);
        textures.upload(slot, 0, 0, 0, SIZE, SIZE, rgba.data(), rgba.size());
        textures.generateMips(slot);
        slots.push_back(slot);
    }
    if (!settle())
    {
        printf("FAIL the encodes did not finish\n");
        return 1;
    }
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const EncodeCase& c = cases[i];
        const std::vector<uint8_t> source = toRgba(c.pixels, c.channels);
        const uint32_t kept = c.expected == asr::TextureFormat::BC4 ? 1 : c.expected == asr::TextureFormat::BC5 ? 2
                            : c.expected == asr::TextureFormat::BC1 ? 3 : 4;
        // The blocks as stored, decoded on the CPU; where the hardware can decode them too,
        // both must agree (BC1 and BC4 leave the rounding of their steps to the hardware).
        const auto decodeMip = [&](uint32_t level, std::vector<uint8_t>& decoded) {
            std::vector<uint8_t> blocks;
            const uint32_t side = SIZE >> level;
            if (!device->readTextureData(slots[i], level, blocks)) return false;
            decoded = decodeBlocks(c.expected, blocks, side, side);
            std::vector<uint8_t> hardware;
            if (device->readTexture(slots[i], level, hardware))
            {
                int worst = 0;
                size_t worstAt = 0;
                for (size_t t = 0; t < size_t(side) * side; ++t)
                    for (uint32_t ch = 0; ch < kept; ++ch)
                    {
                        const int d = std::abs(int(hardware[t * 4 + ch]) - int(decoded[t * 4 + ch]));
                        if (d > worst) { worst = d; worstAt = t; }
                    }
                // BC7 decodes exactly. NVIDIA rounds BC1's thirds its own way, by up to 4, and
                // BC4 and BC5 steps at a higher precision, by 1.
                const int allowed = c.expected == asr::TextureFormat::BC1 ? 8 : c.expected == asr::TextureFormat::BC7 ? 0 : 1;
                if (worst > allowed)
                {
                    const size_t x = worstAt % side, y = worstAt / side;
                    const uint8_t* block = &blocks[((y / 4) * ((side + 3) / 4) + x / 4) * asr::textureBlockBytes(c.expected)];
                    printf("FAIL %s mip %u: the CPU decoder differs from the hardware by %d at %zu,%zu (hw %d %d, cpu %d %d), block", c.name, level, worst, x, y,
                           hardware[worstAt * 4], hardware[worstAt * 4 + 1], decoded[worstAt * 4], decoded[worstAt * 4 + 1]);
                    for (uint32_t k = 0; k < asr::textureBlockBytes(c.expected); ++k) printf(" %02x", block[k]);
                    printf("\n");
                    okay = false;
                }
                decoded = hardware;
            }
            return true;
        };
        std::vector<uint8_t> decoded;
        if (!decodeMip(0, decoded))
        {
            printf("SKIP %s: this device cannot read a texture back\n", c.name);
            continue;
        }
        const double quality = psnr(source, decoded, SIZE * SIZE, kept);

        // Level 1 against a 2x2 box of the source: the mips are made before encoding.
        std::vector<uint8_t> box(size_t(SIZE / 2) * (SIZE / 2) * 4);
        for (uint32_t y = 0; y < SIZE / 2; ++y)
            for (uint32_t x = 0; x < SIZE / 2; ++x)
                for (uint32_t ch = 0; ch < 4; ++ch)
                {
                    const auto at = [&](uint32_t sx, uint32_t sy) { return int(source[(sy * SIZE + sx) * 4 + ch]); };
                    box[(y * (SIZE / 2) + x) * 4 + ch] = uint8_t((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        std::vector<uint8_t> mip;
        const double mipQuality = decodeMip(1, mip) ? psnr(box, mip, (SIZE / 2) * (SIZE / 2), kept) : 0.0;
        const bool pass = quality >= c.minPsnr && mipQuality >= c.minPsnr - 3.0;
        printf("%s %s as %s: %.1f dB (at least %.1f), mip 1 %.1f dB\n", pass ? "ok   " : "FAIL ", c.name, formatName(c.expected), quality, c.minPsnr, mipQuality);
        okay = okay && pass;
    }

    // The disk cache's round trip: an encode read back, then a texture made from those
    // blocks with no encoding, which must hold the very same blocks at every mip.
    {
        const EncodeCase& c = cases[2]; // alpha edges, BC7
        asr::TextureDesc desc;
        desc.width = SIZE;
        desc.height = SIZE;
        desc.mipLevels = MIPS;
        desc.format = c.expected;
        const std::vector<uint8_t> rgba = toRgba(c.pixels, c.channels);
        const uint32_t encodedSlot = textures.create(desc);
        textures.upload(encodedSlot, 0, 0, 0, SIZE, SIZE, rgba.data(), rgba.size());
        constexpr uint64_t TOKEN = 42;
        textures.generateMips(encodedSlot, TOKEN);
        std::vector<asr::TextureQueue::EncodedTexture> encoded;
        for (int i = 0; i < 20 && encoded.empty() && settle(); ++i)
        {
            frames(1);
            textures.takeEncoded(encoded);
        }
        bool roundTrip = encoded.size() == 1 && encoded[0].token == TOKEN && encoded[0].blocks.size() == asr::textureBytes(desc);
        if (roundTrip)
        {
            std::vector<uint8_t> stored;
            roundTrip = device->readTextureData(encodedSlot, 0, stored) &&
                        std::equal(stored.begin(), stored.end(), encoded[0].blocks.begin());
        }
        uint32_t cachedSlot = 0;
        if (roundTrip)
        {
            cachedSlot = textures.createEncoded(desc);
            for (uint32_t level = 0; level < MIPS; ++level)
            {
                textures.uploadBlocks(cachedSlot, desc, level, encoded[0].blocks.data() + asr::textureMipOffset(desc, level, true));
            }
            textures.generateMips(cachedSlot);
            roundTrip = settle();
        }
        for (uint32_t level = 0; roundTrip && level < MIPS; ++level)
        {
            std::vector<uint8_t> stored;
            const uint64_t at = asr::textureMipOffset(desc, level, true);
            roundTrip = device->readTextureData(cachedSlot, level, stored) &&
                        stored.size() == asr::textureMipOffset(desc, level + 1, true) - at &&
                        std::equal(stored.begin(), stored.end(), encoded[0].blocks.begin() + at);
        }
        printf("%s blocks read back and made into a texture again, every mip the same\n", roundTrip ? "ok   " : "FAIL ");
        okay = okay && roundTrip;
    }

    // Speed: a teleport's worth of large textures, at most ENCODE_TEXELS_PER_FRAME a frame.
    constexpr uint32_t BIG = 1024;
    constexpr uint32_t COUNT = 24;
    std::vector<uint8_t> big(size_t(BIG) * BIG * 4);
    std::mt19937 random(11);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t((i & 3) == 3 ? 255 : (random() & 0x3F) + (i / 4 % BIG) / 8);
    for (const asr::TextureFormat format : { asr::TextureFormat::BC1, asr::TextureFormat::BC7 })
    {
        frames(10);
        auto start = Clock::now();
        frames(30);
        const double idle = std::chrono::duration<double>(Clock::now() - start).count() / 30.0;
        asr::TextureDesc desc;
        desc.width = BIG;
        desc.height = BIG;
        desc.mipLevels = 11;
        desc.format = format;
        for (uint32_t i = 0; i < COUNT; ++i)
        {
            const uint32_t slot = textures.create(desc);
            textures.upload(slot, 0, 0, 0, BIG, BIG, big.data(), big.size());
            textures.generateMips(slot);
        }
        start = Clock::now();
        uint32_t count = 0;
        while (rendering && count < 5000)
        {
            frames(1);
            ++count;
            if (device->frameParts().textureCommandsLeft == 0 && textures.waitingBytes() == 0) break;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        const double texels = double(COUNT) * BIG * BIG * 4.0 / 3.0;
        const double extra = std::max(seconds - idle * count, 1e-6);
        printf("speed %s: %u textures of %u in %u frames, %.1f ms a frame against %.1f idle, about %.0f Mtexels/s\n", formatName(format), COUNT, BIG,
               count, seconds * 1000.0 / count, idle * 1000.0, texels / extra / 1e6);
    }
    printf("validation errors: %u\n", device->validationErrors());
    okay = okay && device->validationErrors() == 0;
    printf(okay ? "ALL PASSED\n" : "FAILED\n");
    return okay ? 0 : 1;
}
}
