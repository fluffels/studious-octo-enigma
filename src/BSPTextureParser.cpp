#include "BSPTextureParser.h"

BSPTextureParser::BSPTextureParser(FILE* file, int32_t offset, Palette& palette):
    baseOffset(offset),
    file(file),
    palette(palette)
{
    parseHeader();
    parseTextureHeaders();
    parseTextures();
}

void BSPTextureParser::parseHeader() {
    seek(file, baseOffset);
    readStruct(file, header.numtex);

    auto count = header.numtex;
    auto elementSize = sizeof(int32_t);

    header.offset.resize(count);
    auto readCount = fread_s(
        header.offset.data(),
        count * elementSize,
        elementSize,
        count,
        file
    );
    if (readCount != count) {
        throw runtime_error("unexpected EOF");
    }
}

void BSPTextureParser::parseTextureHeaders() {
    auto count = header.numtex;
    auto elementSize = sizeof(TextureHeader);

    texTypes.resize(count);

    textureHeaders.resize(count);

    for (int i = 0; i < count; i++) {
        auto& textureHeader = textureHeaders[i];

        auto texHeaderOffset = header.offset[i];
        if (texHeaderOffset > 0) {
            auto offset = baseOffset + texHeaderOffset;
            seek(file, offset);
            readStruct(file, textureHeader);
        } else {
            textureHeader = {};
        }
    }
}

void BSPTextureParser::parseTexture(int idx, Texture& texture) {
    auto headerOffset = header.offset[idx];
    auto& header = textureHeaders[idx];
    texture.width = header.width;
    texture.height = header.height;
    auto size = header.width * header.height;

    if ((strncmp(header.name, "clip", 4) == 0) ||
            (strncmp(header.name, "trigger", 7) == 0) ||
            (size == 0)) {
        texTypes[idx] = TEXTYPE::DEBUG;
    } else if (strncmp(header.name, "sky", 3) == 0) {
        texTypes[idx] = TEXTYPE::SKY;
    } else if (strncmp(header.name, "*", 1) == 0) {
        texTypes[idx] = TEXTYPE::FLUID;
    } else {
        texTypes[idx] = TEXTYPE::DEFAULT;
    }

    vector<uint8_t> textureColorIndices(size);

    seek(file, baseOffset + headerOffset + header.offset1);
    fread_s(textureColorIndices.data(), size, size, 1, file);

    texture.texels.resize(size * 4);

    for (uint32_t i = 0; i < size; i++) {
        auto colorIdx = textureColorIndices[i];
        auto paletteColor = palette.colors[colorIdx];
        texture.texels[i*4] = paletteColor.r;
        texture.texels[i*4+1] = paletteColor.g;
        texture.texels[i*4+2] = paletteColor.b;
        texture.texels[i*4+3] = 255;
    }
}

void BSPTextureParser::splitSkyTexture(
    int idx,
    Texture& texture,
    Texture& front,
    Texture& back
) {
    auto& header = textureHeaders[idx];
    auto halfSize = header.width * header.height * 4 / 2;
    front.width = back.width = header.width / 2;
    front.height = back.height = header.height;
    front.texels.resize(halfSize);
    back.texels.resize(halfSize);
    auto src = texture.texels.data();
    auto f = front.texels.data();
    auto b = back.texels.data();
    for (uint32_t y = 0; y < header.height; y++) {
        for (uint32_t x = 0; x < header.width / 2; x++) {
            for (uint32_t c = 0; c < 4; c++) {
                *f++ = *src++;
            }
        }
        for (uint32_t x = header.width / 2; x < header.width; x++) {
            for (uint32_t c = 0; c < 4; c++) {
                *b++ = *src++;
            }
        }
    }

}

void BSPTextureParser::parseTextures() {
    // NOTE(jan): for some reason, textures can sometimes have a zero area.
    // To prevent this causing problems we skip such textures and map
    // their indices to a large texture index so the error is obvious.
    for (int idx = 0; idx < header.numtex; idx++) {
        auto& textureHeader = textureHeaders[idx];
        Texture texture = {};
        parseTexture(idx, texture);
        auto texType = texTypes[idx];
        if (texType == TEXTYPE::DEBUG) {
            texNums[idx] = -1;
        } else if (texType == TEXTYPE::SKY) {
            Texture front = {};
            Texture back = {};
            splitSkyTexture(idx, texture, front, back);
            texNums[idx] = (uint32_t)skyTextures.size();
            skyTextures.push_back(front);
            skyTextures.push_back(back);
        } else if (texType == TEXTYPE::FLUID) {
            texNums[idx] = (uint32_t)fluidTextures.size();
            fluidTextures.push_back(texture);
        } else {
            texNums[idx] = (uint32_t)textures.size();
            textures.push_back(texture);
        }
    }
}
