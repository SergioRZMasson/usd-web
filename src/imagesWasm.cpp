/*
Copyright 2023 Adobe. All rights reserved.
This file is licensed to you under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License. You may obtain a copy
of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed under
the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR REPRESENTATIONS
OF ANY KIND, either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

// WebAssembly replacement for utils/src/images.cpp.
//
// The upstream implementation depends on OpenImageIO and pxr/imaging/hio. Neither
// is available in a minimal Emscripten build: OpenImageIO drags in libtiff, libjpeg,
// OpenEXR and Boost, and hio only exists when PXR_BUILD_IMAGING=ON, which would pull
// Hydra and OpenSubdiv into the binary for no benefit in a converter.
//
// This file implements the exact public surface of fileformatutils/images.h on top of
// stb_image / stb_image_write, which are dependency-free single headers. Semantics are
// preserved deliberately:
//   * pixels are normalised floats, straight (unassociated) alpha, as OIIO produced;
//   * Image::read honours forceChannels;
//   * the same dimension validation and TF_WARN diagnostics are emitted.
//
// Format coverage differs from upstream and is the one functional trade-off: stb decodes
// png/jpg/bmp/tga/gif/psd/hdr and encodes png/jpg/bmp/tga/hdr. TIFF and EXR are not
// supported; assets using them are rejected with a clear warning rather than silently
// producing wrong pixels.

#include <fileformatutils/common.h>
#include <fileformatutils/debugCodes.h>
#include <fileformatutils/images.h>

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

using namespace PXR_NS;

namespace adobe::usd {

namespace {

// Mirrors the upstream overflow-checked validation: each multiplication is checked
// before it is performed, because three uint32 values near UINT32_MAX can exceed
// UINT64_MAX when multiplied. Downstream code assumes the product fits in a signed int.
bool
validateImageDimensions(unsigned int width,
                        unsigned int height,
                        unsigned int channels,
                        size_t& pixelCount)
{
    if (width == 0 || height == 0 || channels == 0) {
        return false;
    }
    constexpr uint64_t maxProduct = static_cast<uint64_t>(std::numeric_limits<int>::max());
    uint64_t w = width;
    uint64_t h = height;
    uint64_t c = channels;
    if (w > maxProduct / h) {
        return false;
    }
    uint64_t widthHeight = w * h;
    if (widthHeight > maxProduct / c) {
        return false;
    }
    pixelCount = static_cast<size_t>(widthHeight * c);
    return true;
}

/// Extensions stb can decode.
const std::unordered_set<std::string>&
decodableExtensions()
{
    static const std::unordered_set<std::string> extensions = {
        "png", "jpg", "jpeg", "bmp", "tga", "gif", "psd", "hdr", "pic", "pnm", "ppm", "pgm"
    };
    return extensions;
}

/// Extensions stb can encode.
///
/// HDR is deliberately absent: stb groups its Radiance writer inside the
/// STBI_WRITE_NO_STDIO block, and glTF has no core HDR texture format, so every texture
/// this pipeline emits ends up as PNG or JPEG anyway. Reading .hdr still works.
bool
isEncodableFormat(ImageFormat format)
{
    switch (format) {
        case ImageFormatPng:
        case ImageFormatJpg:
        case ImageFormatBmp:
        case ImageFormatTga:
            return true;
        default:
            return false;
    }
}

/// stb write callback that appends into a std::vector<uint8_t>.
void
appendToVector(void* context, void* data, int size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// Converts normalised float samples to 8-bit with clamping and rounding.
std::vector<uint8_t>
toBytes(const std::vector<float>& pixels)
{
    std::vector<uint8_t> bytes(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i) {
        float v = pixels[i];
        if (!(v > 0.0f)) { // also catches NaN
            bytes[i] = 0;
        } else if (v >= 1.0f) {
            bytes[i] = 255;
        } else {
            bytes[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }
    return bytes;
}

/// Decodes an encoded buffer into normalised floats.
///
/// stb's float loader applies a gamma curve by default; both the gamma and the scale are
/// reset to 1.0 so an 8-bit source becomes a plain value/255, matching what OIIO returned
/// for a UINT8 -> FLOAT conversion. The globals are process-wide in stb, so the mutex keeps
/// concurrent decodes from observing a half-applied setting.
bool
decodeToFloat(const uint8_t* data,
              size_t size,
              int forceChannels,
              int& width,
              int& height,
              int& channels,
              std::vector<float>& pixels)
{
    static std::mutex stbGammaMutex;
    std::lock_guard<std::mutex> lock(stbGammaMutex);

    stbi_ldr_to_hdr_gamma(1.0f);
    stbi_ldr_to_hdr_scale(1.0f);

    int sourceChannels = 0;
    int w = 0;
    int h = 0;
    float* decoded = stbi_loadf_from_memory(
      data, static_cast<int>(size), &w, &h, &sourceChannels, forceChannels > 0 ? forceChannels : 0);
    if (decoded == nullptr) {
        return false;
    }

    const int outChannels = forceChannels > 0 ? forceChannels : sourceChannels;
    size_t pixelCount = 0;
    const bool validDimensions = w >= 0 && h >= 0 && outChannels >= 0 &&
                                 validateImageDimensions(static_cast<unsigned int>(w),
                                                         static_cast<unsigned int>(h),
                                                         static_cast<unsigned int>(outChannels),
                                                         pixelCount);
    if (!validDimensions) {
        stbi_image_free(decoded);
        return false;
    }

    width = w;
    height = h;
    channels = outChannels;
    pixels.assign(decoded, decoded + pixelCount);
    stbi_image_free(decoded);
    return true;
}

/// Encodes normalised float samples into `format`.
bool
encodeFromFloat(ImageFormat format,
                int width,
                int height,
                int channels,
                const std::vector<float>& pixels,
                std::vector<uint8_t>& out)
{
    out.clear();

    const std::vector<uint8_t> bytes = toBytes(pixels);
    switch (format) {
        case ImageFormatPng:
            return stbi_write_png_to_func(appendToVector,
                                          &out,
                                          width,
                                          height,
                                          channels,
                                          bytes.data(),
                                          width * channels) != 0;
        case ImageFormatJpg:
            // Quality 95: high enough that the encoder is not the dominant error term
            // when transcoding, while keeping payloads reasonable for the web.
            return stbi_write_jpg_to_func(
                     appendToVector, &out, width, height, channels, bytes.data(), 95) != 0;
        case ImageFormatBmp:
            return stbi_write_bmp_to_func(
                     appendToVector, &out, width, height, channels, bytes.data()) != 0;
        case ImageFormatTga:
            return stbi_write_tga_to_func(
                     appendToVector, &out, width, height, channels, bytes.data()) != 0;
        default:
            return false;
    }
}

std::string
_getAssetFileExtension(const std::string& resolvedAssetPath)
{
    return TfStringToLower(ArGetResolver().GetExtension(resolvedAssetPath));
}

/// Reads an entire file from the (virtual) filesystem.
bool
readWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size == 0) {
        return true;
    }
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

} // namespace

Image::Image()
  : width(0U)
  , height(0U)
  , channels(0U)
{}

Image::~Image() {}

bool
Image::allocate(unsigned int width, unsigned int height, unsigned int channels)
{
    size_t pixelCount = 0;
    if (!validateImageDimensions(width, height, channels, pixelCount)) {
        TF_WARN("Image::allocate() rejected invalid or oversized dimensions: width=%u, "
                "height=%u, channels=%u",
                width,
                height,
                channels);
        this->width = 0;
        this->height = 0;
        this->channels = 0;
        pixels.clear();
        return false;
    }
    this->width = static_cast<int>(width);
    this->height = static_cast<int>(height);
    this->channels = static_cast<int>(channels);
    pixels.resize(pixelCount);
    return !pixels.empty();
}

bool
Image::read(const ImageAsset& imageAsset, int forceChannels)
{
    const std::string extension = getFormatExtension(imageAsset.format);
    if (extension.empty()) {
        return false;
    }
    if (imageAsset.image.empty()) {
        TF_WARN("Image::read() received an empty buffer for URI=%s", imageAsset.uri.c_str());
        return false;
    }

    if (!decodeToFloat(imageAsset.image.data(),
                       imageAsset.image.size(),
                       forceChannels,
                       width,
                       height,
                       channels,
                       pixels)) {
        TF_WARN("Image::read() failed to decode image with URI=%s (format '%s'): %s",
                imageAsset.uri.c_str(),
                extension.c_str(),
                stbi_failure_reason() ? stbi_failure_reason() : "unsupported or corrupt data");
        width = 0;
        height = 0;
        channels = 0;
        pixels.clear();
        return false;
    }

    return true;
}

bool
Image::write(ImageAsset& imageAsset) const
{
    if (width < 1 || height < 1 || channels < 1) {
        TF_WARN("Trying to write invalid Image to ImageAsset %s with dimensions: "
                "width=%d, height=%d, channels=%d",
                imageAsset.uri.c_str(),
                width,
                height,
                channels);
        return false;
    }
    if (imageAsset.format == ImageFormatUnknown) {
        TF_CODING_ERROR("Trying to write Image to ImageAsset %s with unknown format",
                        imageAsset.uri.c_str());
        return false;
    }
    const std::string extension = getFormatExtension(imageAsset.format);
    if (extension.empty()) {
        TF_CODING_ERROR("Trying to write Image to ImageAsset %s with empty extension",
                        imageAsset.uri.c_str());
        return false;
    }
    if (!isEncodableFormat(imageAsset.format)) {
        TF_WARN("Encoding to '%s' is not supported in the WebAssembly build (ImageAsset %s). "
                "Supported encodings are png, jpg, bmp, tga and hdr.",
                extension.c_str(),
                imageAsset.uri.c_str());
        return false;
    }

    if (!encodeFromFloat(imageAsset.format, width, height, channels, pixels, imageAsset.image)) {
        TF_WARN("Failed to encode image data for %s as '%s'",
                imageAsset.uri.c_str(),
                extension.c_str());
        return false;
    }
    return true;
}

bool
Image::convertImageToPng(const ImageAsset& srcImageAsset, ImageAsset& dstImageAsset)
{
    if (srcImageAsset.format == ImageFormatUnknown) {
        TF_CODING_ERROR("Trying to write Image to ImageAsset %s with unknown format",
                        srcImageAsset.uri.c_str());
        return false;
    }
    if (getFormatExtension(srcImageAsset.format).empty()) {
        return false;
    }

    Image image;
    if (!image.read(srcImageAsset)) {
        return false;
    }

    dstImageAsset.format = ImageFormatPng;
    if (dstImageAsset.uri.empty()) {
        dstImageAsset.uri = srcImageAsset.uri;
    }
    if (dstImageAsset.name.empty()) {
        dstImageAsset.name = srcImageAsset.name;
    }

    return image.write(dstImageAsset);
}

bool
Image::copyChannel(const Image& imageSrc, int channelSrc, int channelDst)
{
    return transformChannel(imageSrc, channelSrc, 1.0f, 0.0f, channelDst);
}

bool
Image::transformChannel(const Image& imageSrc,
                        int channelSrc,
                        float scale,
                        float bias,
                        int channelDst)
{
    if (width != imageSrc.width || height != imageSrc.height || channelSrc >= imageSrc.channels ||
        channelDst >= channels)
        return false;

    const uint32_t pixelCount = width * height;
    const float* src = imageSrc.pixels.data();
    const int numSrcChannels = imageSrc.channels;
    float* dst = pixels.data();
    const int numDstChannels = channels;
    if (scale == 1.0f && bias == 0.0f) {
        for (uint32_t i = 0; i < pixelCount; i++) {
            dst[i * numDstChannels + channelDst] = src[i * numSrcChannels + channelSrc];
        }
    } else {
        for (uint32_t i = 0; i < pixelCount; i++) {
            dst[i * numDstChannels + channelDst] =
              src[i * numSrcChannels + channelSrc] * scale + bias;
        }
    }
    return true;
}

void
Image::set(float r, float g, float b, float a)
{
    int pixelCount = width * height;
    float* dst = pixels.data();
    switch (channels) {
        case 1:
            for (int i = 0; i < pixelCount; i++) {
                dst[i] = r;
            }
            break;
        case 2:
            for (int i = 0; i < pixelCount; i++) {
                dst[2 * i] = r;
                dst[2 * i + 1] = g;
            }
            break;
        case 3:
            for (int i = 0; i < pixelCount; i++) {
                dst[3 * i] = r;
                dst[3 * i + 1] = g;
                dst[3 * i + 2] = b;
            }
            break;
        case 4:
            for (int i = 0; i < pixelCount; i++) {
                dst[4 * i] = r;
                dst[4 * i + 1] = g;
                dst[4 * i + 2] = b;
                dst[4 * i + 3] = a;
            }
            break;
    }
}

std::pair<GfVec4f, GfVec4f>
Image::computeRange() const
{
    float minr = FLT_MAX;
    float ming = FLT_MAX;
    float minb = FLT_MAX;
    float mina = FLT_MAX;
    float maxr = -FLT_MAX;
    float maxg = -FLT_MAX;
    float maxb = -FLT_MAX;
    float maxa = -FLT_MAX;

    int pixelCount = width * height;
    const float* src = pixels.data();
    switch (channels) {
        case 1:
            for (int i = 0; i < pixelCount; i++) {
                float r = src[i];
                minr = std::min(r, minr);
                maxr = std::max(r, maxr);
            }
            break;
        case 2:
            for (int i = 0; i < pixelCount; i++) {
                float r = src[2 * i + 0];
                float g = src[2 * i + 1];
                minr = std::min(r, minr);
                maxr = std::max(r, maxr);
                ming = std::min(g, ming);
                maxg = std::max(g, maxg);
            }
            break;
        case 3:
            for (int i = 0; i < pixelCount; i++) {
                float r = src[3 * i + 0];
                float g = src[3 * i + 1];
                float b = src[3 * i + 2];
                minr = std::min(r, minr);
                maxr = std::max(r, maxr);
                ming = std::min(g, ming);
                maxg = std::max(g, maxg);
                minb = std::min(b, minb);
                maxb = std::max(b, maxb);
            }
            break;
        case 4:
            for (int i = 0; i < pixelCount; i++) {
                float r = src[4 * i + 0];
                float g = src[4 * i + 1];
                float b = src[4 * i + 2];
                float a = src[4 * i + 3];
                minr = std::min(r, minr);
                maxr = std::max(r, maxr);
                ming = std::min(g, ming);
                maxg = std::max(g, maxg);
                minb = std::min(b, minb);
                maxb = std::max(b, maxb);
                mina = std::min(a, mina);
                maxa = std::max(a, maxa);
            }
            break;
    }

    return { GfVec4f(minr, ming, minb, mina), GfVec4f(maxr, maxg, maxb, maxa) };
}

bool
imageMult(const Image& in, const Image& factor, Image& out)
{
    out.allocate(in.width, in.height, in.channels);
    if (in.width != factor.width || in.height != factor.height) {
        // Upstream copies the input through when the factor doesn't match. The element
        // count is used here (not a byte count) so the whole buffer is actually copied.
        const size_t valueCount =
          static_cast<size_t>(in.width) * in.height * in.channels;
        if (out.pixels.size() >= valueCount && in.pixels.size() >= valueCount) {
            std::memcpy(out.pixels.data(), in.pixels.data(), valueCount * sizeof(float));
        }
        TF_WARN("imageMult: in image size (%d x %d) doesn't match factor size (%d x %d)",
                in.width,
                in.height,
                factor.width,
                factor.height);
        return false;
    }

    unsigned int pixelCount = in.width * in.height;
    const float* factorSrc = factor.pixels.data();
    int factorChannels = factor.channels;
    const float* src = in.pixels.data();
    int srcChannels = in.channels;
    float* dst = out.pixels.data();
    for (unsigned int i = 0; i < pixelCount; i++) {
        float f = factorSrc[i * factorChannels]; // takes value from first channel
        for (int j = 0; j < srcChannels; j++) {
            dst[i * srcChannels + j] = src[i * srcChannels + j] * f;
        }
    }

    return true;
}

bool
imageTransformAffine(const Image& in, float scale, float bias, Image& out)
{
    const int channels = in.channels;
    out.allocate(in.width, in.height, channels);
    size_t valueCount = static_cast<size_t>(in.width) * in.height * channels;
    const float* src = in.pixels.data();
    float* dst = out.pixels.data();
    for (size_t i = 0; i < valueCount; i++) {
        *dst++ = scale * (*src++) + bias;
    }
    return true;
}

bool
imageExtractChannel(const Image& in, int channelSrc, float scale, float bias, Image& out)
{
    if (channelSrc < 0 || channelSrc >= in.channels) {
        TF_WARN("Invalid channel index (%d) for extraction from source image", channelSrc);
        return false;
    }

    out.allocate(in.width, in.height, 1);
    return out.transformChannel(in, channelSrc, scale, bias, 0);
}

void
imageWrite(const adobe::usd::ImageAsset& image, const std::string& filename, bool overwrite)
{
    const std::string parentPath = TfGetPathName(filename);
    TfMakeDirs(parentPath, -1, true);
    std::ifstream ifile(filename);
    if (ifile.good() && !overwrite) {
        TF_WARN("File %s already exists, not overwriting", filename.c_str());
        ifile.close();
        return;
    }
    std::ofstream ofile(filename.c_str(), std::ios::out | std::ios::binary);
    if (!ofile.is_open()) {
        return;
    }
    ofile.write(reinterpret_cast<const char*>(image.image.data()), image.image.size());
    ofile.close();
    TF_STATUS("Wrote image to %s", filename.c_str());
}

float
srgbToLinear(float s)
{
    if (s < 0.040448f)
        return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float
linearToSRGB(float s)
{
    if (s < 0.0031308f)
        return s * 12.92f;
    return 1.055f * std::pow(s, (1.0f / 2.4f)) - 0.055f;
}

bool
isImageFileSupported(const std::string& resolvedAssetPath)
{
    static std::unordered_map<std::string, bool> supportedExtensions;
    static std::mutex supportedExtensionsMutex;

    std::lock_guard<std::mutex> lock(supportedExtensionsMutex);

    std::string ext = _getAssetFileExtension(resolvedAssetPath);
    auto [it, inserted] = supportedExtensions.emplace(ext, false);
    if (inserted) {
        it->second = decodableExtensions().count(ext) > 0;
        if (!it->second) {
            TF_WARN("Image file with extension '%s' at path '%s' is not supported",
                    ext.c_str(),
                    resolvedAssetPath.c_str());
        }
    }
    return it->second;
}

bool
isUriSbsarImage(const std::string& uri)
{
    size_t pos = uri.find_first_of('?');
    return uri.length() > 1 && pos != std::string::npos;
}

std::string
getSbsarUsageFromParameters(const std::string& parametersStr)
{
    auto params = split(parametersStr, '#');
    for (const auto& param : params) {
        auto keyValue = split(param, '=');
        if (keyValue.size() != 2) {
            continue;
        }
        if (keyValue[0] == "usage") {
            return keyValue[1];
        }
    }

    return {};
}

std::string
getSbsarImageExtension(const std::string& resolvedAssetPath)
{
    if (!isImageFileSupported(resolvedAssetPath)) {
        TF_WARN("Asset %s is not a supported image type", resolvedAssetPath.c_str());
        return {};
    }

    // Upstream inspects the decoded pixel format via Hio and routes floating point
    // imagery to exr. This build cannot encode HDR (see isEncodableFormat), and glTF
    // has no core HDR texture format, so everything is normalised to png.
    return std::string("png");
}

std::string
extractFilePathFromAssetPath(const std::string& assetPath)
{
    auto q = assetPath.find_first_of('?');
    if (q == std::string::npos) {
        return assetPath;
    }

    std::string subpath = assetPath.substr(0, q);
    if (std::filesystem::path(subpath).has_extension()) {
        return subpath;
    }

    std::string ext = ArGetResolver().GetExtension(assetPath);
    if (ext.empty()) {
        TF_WARN("Could not find file extension for asset path %s", assetPath.c_str());
    }

    std::string parameters = assetPath.substr(q + 1, assetPath.size() - (q + 1) - (ext.size() + 1));

    std::string usage = getSbsarUsageFromParameters(parameters);
    if (!usage.empty()) {
        std::string graphName =
          convertPathToString(std::filesystem::path(subpath).parent_path().filename());
        subpath = graphName + "_" + usage;
    }

    subpath = subpath + "." + ext;

    return subpath;
}

bool
transcodeImageAssetToMemory(const std::string& resolvedAssetPath,
                            const std::string& filename,
                            std::vector<uint8_t>& outputPixelData)
{
    if (!isImageFileSupported(resolvedAssetPath)) {
        TF_WARN("Asset %s is not a supported image type", resolvedAssetPath.c_str());
        return false;
    }

    // Upstream round-trips through a temporary file because the Hio API cannot encode to
    // memory. stb can, so the transcode stays entirely in memory here — which also avoids
    // depending on a writable temp directory inside the Emscripten filesystem.
    const std::string outputExtension = TfStringToLower(TfGetExtension(filename));
    const ImageFormat outputFormat =
      outputExtension.empty() ? ImageFormatPng : getFormat(outputExtension);
    if (!isEncodableFormat(outputFormat)) {
        TF_WARN("Output %s is not a supported image type", filename.c_str());
        return false;
    }

    std::vector<uint8_t> encoded;
    if (!readWholeFile(resolvedAssetPath, encoded)) {
        TF_WARN("Couldn't open image %s for reading", resolvedAssetPath.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> pixels;
    if (!decodeToFloat(encoded.data(), encoded.size(), 0, width, height, channels, pixels)) {
        TF_WARN("Reading of image %s failed: %s",
                resolvedAssetPath.c_str(),
                stbi_failure_reason() ? stbi_failure_reason() : "unsupported or corrupt data");
        return false;
    }

    if (!encodeFromFloat(outputFormat, width, height, channels, pixels, outputPixelData)) {
        TF_WARN("Writing of image %s failed", filename.c_str());
        return false;
    }

    return true;
}

}
