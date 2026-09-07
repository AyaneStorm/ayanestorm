/**
 * @file ascolorlut.cpp
 * @author chanayane@firestorm
 * @brief Validated .cube import, folder discovery, and one cached GPU LUT.
 */
#include "llviewerprecompiledheaders.h"
#include "ascolorlut.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llglslshader.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llrender.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <vector>

namespace
{
    struct Cube
    {
        S32 size = 0;
        bool one_dimensional = false;
        F32 minimum[3] = {0.f, 0.f, 0.f};
        F32 maximum[3] = {1.f, 1.f, 1.f};
        std::vector<F32> rgb;
    };

    Cube sCube;
    std::string sName;
    std::string sError;
    bool sDirty = true;
    GLuint sTexture = 0;
    S32 sChannel3D = -1;
    S32 sChannel1D = -1;
    bool sBound = false;
    const LLStaticHashedString sSampler("as_color_grade_lut");
    const LLStaticHashedString sSampler1D("as_color_grade_lut_1d");
    const LLStaticHashedString sType("as_color_grade_lut_type");
    const LLStaticHashedString sStrength("as_color_grade_lut_strength");
    const LLStaticHashedString sDomainMin("as_color_grade_lut_min");
    const LLStaticHashedString sDomainScale("as_color_grade_lut_domain_scale");
    const LLStaticHashedString sTexel("as_color_grade_lut_texel");

    std::string userDir()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "luts");
    }

    std::string appDir()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "lut");
    }

    std::string pathIn(const std::string& dir, const std::string& name)
    {
        return dir + gDirUtilp->getDirDelimiter() + name;
    }

    // User copies take precedence over application-directory files of the same name.
    std::string resolve(const std::string& name)
    {
        if (!ASColorLUT::validName(name)) return {};
        const std::string user = pathIn(userDir(), name);
        return LLFile::isfile(user) ? user : pathIn(appDir(), name);
    }

    void report(const std::string& name, const std::string& reason)
    {
        LLSD args;
        args["FILE"] = name;
        args["REASON"] = reason;
        LLNotificationsUtil::add("ASColorGradeLUTFailed", args);
    }

    bool endOfLine(std::istringstream& line)
    {
        line >> std::ws;
        return line.eof();
    }

    // Read only the header during discovery; table data is loaded on selection.
    std::string displayName(const std::string& name)
    {
        llifstream file(resolve(name), std::ios::binary);
        std::string text;
        std::string preset;
        bool first_line = true;
        while (std::getline(file, text))
        {
            if (text.size() > 4096) break;
            if (first_line && text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
            first_line = false;
            std::istringstream line(text);
            line.imbue(std::locale::classic());
            std::string token;
            if (!(line >> token)) continue;
            if (token[0] == '#')
            {
                // Keep the first nonempty Preset comment, but let TITLE take precedence.
                std::string comment = text.substr(text.find('#') + 1);
                LLStringUtil::trim(comment);
                if (preset.empty() && comment.compare(0, 7, "Preset:") == 0)
                {
                    preset = comment.substr(7);
                    LLStringUtil::trim(preset);
                }
                continue;
            }
            if (token == "TITLE")
            {
                std::string title;
                line >> std::ws;
                if (line.peek() == '"')
                {
                    if (!(line >> std::quoted(title))) continue;
                }
                else std::getline(line, title);
                const size_t comment = title.find('#');
                if (comment != std::string::npos) title.resize(comment);
                LLStringUtil::trim(title);
                if (!title.empty()) return title;
                continue;
            }
            if (token != "LUT_3D_SIZE" && token != "LUT_1D_SIZE" &&
                token != "DOMAIN_MIN" && token != "DOMAIN_MAX") break;
        }
        return preset.empty() ? name : preset;
    }

    std::string safeDisplayName(const std::string& name)
    {
        try { return displayName(name); }
        catch (...) { return name; }
    }

    bool readTriple(std::istringstream& line, F32* values)
    {
        return bool(line >> values[0] >> values[1] >> values[2]) && endOfLine(line) &&
            std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
    }

    // Accept one 3D table, red changing fastest, with optional input domains.
    // Bound allocations and reject malformed data before it reaches OpenGL.
    bool readCube(const std::string& path, Cube& cube, std::string& error)
    {
        llifstream file(path, std::ios::binary);
        if (!file.is_open()) { error = "File is missing or cannot be read."; return false; }
        file.seekg(0, std::ios::end);
        const auto length = file.tellg();
        if (length < 0 || length > 128 * 1024 * 1024)
        { error = "File exceeds the 128 MiB limit or cannot be read."; return false; }
        file.seekg(0);
        bool have_min = false, have_max = false, have_title = false;
        std::string text;
        S32 line_number = 0;
        auto fail = [&](const std::string& reason)
        {
            error = "Line " + std::to_string(line_number) + ": " + reason;
            return false;
        };
        while (std::getline(file, text))
        {
            ++line_number;
            if (text.size() > 4096) return fail("Line is too long.");
            if (line_number == 1 && text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
            // Comments may follow values; a # inside a quoted title is literal.
            bool quoted = false;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '"') quoted = !quoted;
                if (text[i] == '#' && !quoted) { text.resize(i); break; }
            }
            std::istringstream line(text);
            line.imbue(std::locale::classic());
            std::string token;
            if (!(line >> token)) continue;
            if (token == "TITLE")
            {
                std::string title;
                line >> std::ws;
                if (have_title || !cube.rgb.empty()) return fail("Repeated or misplaced TITLE.");
                if (line.peek() == '"')
                {
                    if (!(line >> std::quoted(title)) || !endOfLine(line)) return fail("Invalid TITLE.");
                }
                else
                {
                    std::getline(line, title);
                    LLStringUtil::trim(title);
                    if (title.empty()) return fail("Invalid TITLE.");
                }
                have_title = true;
            }
            else if (token == "LUT_3D_SIZE" || token == "LUT_1D_SIZE")
            {
                cube.one_dimensional = token == "LUT_1D_SIZE";
                const S32 maximum = cube.one_dimensional ? 65536 : 128;
                if (cube.size || !(line >> cube.size) || !endOfLine(line) || cube.size < 2 || cube.size > maximum)
                    return fail(token + " must occur once and have a supported size.");
                const size_t entries = cube.one_dimensional ? cube.size : size_t(cube.size) * cube.size * cube.size;
                cube.rgb.reserve(entries * 3);
            }
            else if (token == "DOMAIN_MIN" || token == "DOMAIN_MAX")
            {
                bool& have = token == "DOMAIN_MIN" ? have_min : have_max;
                F32* values = token == "DOMAIN_MIN" ? cube.minimum : cube.maximum;
                if (have || !cube.rgb.empty() || !readTriple(line, values)) return fail("Invalid or repeated domain.");
                have = true;
            }
            else
            {
                if (token.compare(0, 4, "LUT_") == 0)
                    return fail("Only a single 1D or 3D .cube table is supported; combined shapers and INPUT_RANGE directives are unsupported.");
                if (!cube.size) return fail("Expected LUT_1D_SIZE or LUT_3D_SIZE before table data.");
                const size_t entries = cube.one_dimensional ? cube.size : size_t(cube.size) * cube.size * cube.size;
                if (cube.rgb.size() >= entries * 3)
                    return fail("Too many table entries.");
                line.clear();
                line.str(text);
                F32 values[3];
                if (!readTriple(line, values)) return fail("Expected three finite RGB numbers.");
                for (F32 value : values)
                {
                    if (std::abs(value) > 65504.f) return fail("RGB value exceeds half-float storage range.");
                    cube.rgb.push_back(value);
                }
            }
        }
        if (!file.eof()) return fail("Error reading file.");
        const size_t entries = cube.one_dimensional ? cube.size : size_t(cube.size) * cube.size * cube.size;
        if (!cube.size || cube.rgb.size() != entries * 3)
            return fail("Missing or incomplete LUT table.");
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 span = cube.maximum[i] - cube.minimum[i];
            if (!(span > 0.f) || !std::isfinite(span) || !std::isfinite(1.f / span))
                return fail("DOMAIN_MAX must be greater than DOMAIN_MIN with a usable range.");
        }
        return true;
    }

    bool safeReadCube(const std::string& path, Cube& cube, std::string& error)
    {
        try { return readCube(path, cube, error); }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory to load this LUT.";
        }
        catch (const std::exception&)
        {
            error = "The LUT parser encountered an unexpected error.";
        }
        catch (...)
        {
            error = "The LUT parser encountered an unknown error.";
        }
        return false;
    }

    void releaseTexture()
    {
        ASColorLUT::unbind();
        if (sTexture) glDeleteTextures(1, &sTexture);
        sTexture = 0;
    }

    // No directory scanning, parsing, or uploading on unchanged frames.
    void prepare()
    {
        const std::string name = gSavedSettings.getString("ASColorGradeLUTFile");
        if (!sDirty && name == sName) return;
        sDirty = false;
        sName = name;
        sError.clear();
        releaseTexture();
        sCube = Cube();
        if (name.empty()) return;
        Cube candidate;
        if (!ASColorLUT::validName(name)) sError = "Select a .cube filename without a path.";
        else if (safeReadCube(resolve(name), candidate, sError)) sCube = std::move(candidate);
        if (!sError.empty()) report(name, sError);
    }

    void importFile(const std::vector<std::string>& filenames)
    {
        if (filenames.empty()) return;
        const std::string& source = filenames.front();
        std::string name = gDirUtilp->getBaseFileName(source);
        Cube candidate;
        std::string error;
        if (!ASColorLUT::validName(name)) { report(name, "Select a .cube file."); return; }
        if (!safeReadCube(source, candidate, error)) { report(name, error); return; }
        LLFile::mkdir(userDir());
        if (!LLFile::isdir(userDir())) { report(name, "Cannot create the user LUT folder."); return; }
        // Preserve existing files on import; duplicate basenames get a numeric suffix.
        const std::string stem = gDirUtilp->getBaseFileName(source, true);
        for (S32 suffix = 1; LLFile::isfile(pathIn(userDir(), name)) || LLFile::isfile(pathIn(appDir(), name)); ++suffix)
            name = stem + " (" + std::to_string(suffix) + ").cube";
        if (!LLFile::copy(source, pathIn(userDir(), name))) { report(name, "Cannot copy the LUT into the user folder."); return; }
        sDirty = true;
        gSavedSettings.setString("ASColorGradeLUTFile", name);
        gSavedSettings.setBOOL("ASColorGradeLUTEnabled", true);
    }
}

bool ASColorLUT::validName(const std::string& name)
{
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : name) if (c < 32) return false;
    return LLStringUtil::compareInsensitive(gDirUtilp->getExtension(name), "cube") == 0;
}

bool ASColorLUT::configureShader(LLGLSLShader& shader)
{
    // The viewer only assigns channels to reserved samplers. Append our private
    // sampler explicitly so it never aliases the scene/depth 2D samplers, even disabled.
    sChannel3D = shader.mActiveTextureChannels;
    sChannel1D = sChannel3D + 1;
    if (shader.getUniformLocation(sSampler) < 0 || shader.getUniformLocation(sSampler1D) < 0 ||
        sChannel3D < 0 || sChannel1D >= gGLManager.mNumTextureImageUnits ||
        sChannel1D >= S32(LL_NUM_TEXTURE_LAYERS))
    {
        sChannel3D = sChannel1D = -1;
        return false;
    }
    shader.bind();
    shader.uniform1i(sSampler, sChannel3D);
    shader.uniform1i(sSampler1D, sChannel1D);
    shader.unbind();
    shader.mActiveTextureChannels += 2;
    return true;
}

void ASColorLUT::bind(LLGLSLShader& shader)
{
    shader.uniform1f(sStrength, 0.f);
    shader.uniform1i(sType, 0);
    if (!gSavedSettings.getBOOL("ASColorGradeLUTEnabled")) return;
    const F32 strength = llclamp(gSavedSettings.getF32("ASColorGradeLUTStrength") * .01f, 0.f, 1.f);
    if (!(strength > 0.f)) return;
    prepare();
    if (!sCube.size || !sError.empty() || sChannel3D < 0 || sChannel1D < 0) return;
    const S32 channel = sCube.one_dimensional ? sChannel1D : sChannel3D;
    const LLTexUnit::eTextureType texture_type = sCube.one_dimensional ? LLTexUnit::TT_TEXTURE : LLTexUnit::TT_TEXTURE_3D;
    const GLenum target = sCube.one_dimensional ? GL_TEXTURE_2D : GL_TEXTURE_3D;
    LLTexUnit* unit = gGL.getTexUnit(channel);
    if (!sTexture)
    {
        GLint max_size = 0;
        glGetIntegerv(sCube.one_dimensional ? GL_MAX_TEXTURE_SIZE : GL_MAX_3D_TEXTURE_SIZE, &max_size);
        if (sCube.size > max_size)
        {
            sError = "This GPU does not support the LUT dimensions.";
            report(sName, sError);
            return;
        }
        glGenTextures(1, &sTexture);
        unit->bindManual(texture_type, sTexture);
        sBound = true;
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!sCube.one_dimensional) glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        // Upload client memory independently of any previous pixel-unpack state.
        const GLenum stores[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
            GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES};
        GLint saved[7], unpack_buffer;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        for (S32 i = 0; i < 7; ++i)
        {
            glGetIntegerv(stores[i], &saved[i]);
            glPixelStorei(stores[i], i == 0 ? 1 : 0);
        }
        if (sCube.one_dimensional)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, sCube.size, 1, 0, GL_RGB, GL_FLOAT, sCube.rgb.data());
        else
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, sCube.size, sCube.size, sCube.size,
                         0, GL_RGB, GL_FLOAT, sCube.rgb.data());
        for (S32 i = 0; i < 7; ++i) glPixelStorei(stores[i], saved[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
        GLint width = 0;
        glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &width);
        if (width != sCube.size)
        {
            releaseTexture();
            sError = "The GPU could not allocate the LUT texture.";
            report(sName, sError);
            return;
        }
    }
    else
    {
        unit->bindManual(texture_type, sTexture);
        sBound = true;
    }
    shader.uniform3fv(sDomainMin, 1, sCube.minimum);
    shader.uniform3f(sDomainScale, 1.f / (sCube.maximum[0] - sCube.minimum[0]),
        1.f / (sCube.maximum[1] - sCube.minimum[1]), 1.f / (sCube.maximum[2] - sCube.minimum[2]));
    shader.uniform2f(sTexel, F32(sCube.size - 1) / sCube.size, .5f / sCube.size);
    shader.uniform1i(sType, sCube.one_dimensional ? 1 : 3);
    shader.uniform1f(sStrength, strength);
}

void ASColorLUT::unbind()
{
    if (sBound)
    {
        const S32 channel = sCube.one_dimensional ? sChannel1D : sChannel3D;
        if (channel >= 0) gGL.getTexUnit(channel)->unbind(
            sCube.one_dimensional ? LLTexUnit::TT_TEXTURE : LLTexUnit::TT_TEXTURE_3D);
    }
    sBound = false;
}

void ASColorLUT::unload()
{
    releaseTexture();
    sChannel3D = sChannel1D = -1;
    // Reload the selected table after shader/context recreation, including prior failures.
    sDirty = true;
}

void ASColorLUT::refreshPanel(LLPanel& panel)
{
    LLComboBox* combo = panel.getChild<LLComboBox>("lut_file");
    combo->removeall();
    combo->add("None", LLSD(""));
    std::map<std::string, std::string> names;
    for (const std::string& dir : {appDir(), userDir()})
    {
        LLDirIterator iter(dir, "*");
        std::string name;
        while (iter.next(name))
        {
            if (!validName(name) || !LLFile::isfile(pathIn(dir, name))) continue;
            std::string key = name;
            LLStringUtil::toLower(key);
            names[key] = name;
        }
    }
    std::vector<std::pair<std::string, std::string>> sorted_names;
    for (const auto& name : names) sorted_names.emplace_back(safeDisplayName(name.second), name.second);
    std::sort(sorted_names.begin(), sorted_names.end(), [](const auto& left, const auto& right)
    {
        const S32 label_order = LLStringUtil::compareInsensitive(left.first, right.first);
        return label_order != 0 ? label_order < 0 : LLStringUtil::compareInsensitive(left.second, right.second) < 0;
    });
    for (const auto& name : sorted_names) combo->add(name.first, LLSD(name.second));
    panel.getChild<LLTextBox>("lut_status")->setText(LLStringExplicit(""));
    updatePanel(panel);
}

void ASColorLUT::updatePanel(LLPanel& panel)
{
    prepare();
    LLComboBox* combo = panel.getChild<LLComboBox>("lut_file");
    const std::string name = gSavedSettings.getString("ASColorGradeLUTFile");
    std::string status;
    if (!sError.empty()) status = name + " bypassed: " + sError;
    else if (name.empty()) status = "Import a .cube file or copy files into the LUT folder.";
    else if (sCube.one_dimensional) status = name + " (1D, " + std::to_string(sCube.size) + ")";
    else status = name + " (" + std::to_string(sCube.size) + " x " +
        std::to_string(sCube.size) + " x " + std::to_string(sCube.size) + ")";
    LLTextBox* status_text = panel.getChild<LLTextBox>("lut_status");
    if (status_text->getValue().asString() != status)
    {
        // Sync only when the loaded selection changes, preserving keyboard
        // navigation in an open dropdown between commits.
        // Labels can differ from filenames or repeat; identify entries by their stored value.
        if (!name.empty() && !combo->getItemByValue(LLSD(name)))
            combo->add(safeDisplayName(name), LLSD(name));
        combo->setValue(LLSD(name));
        status_text->setText(status);
    }
    const S32 index = combo->getCurrentIndex();
    panel.getChild<LLButton>("lut_previous")->setEnabled(index > 0);
    panel.getChild<LLButton>("lut_next")->setEnabled(index >= 0 && index + 1 < combo->getItemCount());
}

void ASColorLUT::initPanel(LLPanel& panel)
{
    panel.getChild<LLComboBox>("lut_file")->setCommitCallback([](LLUICtrl* control, const LLSD&)
    {
        gSavedSettings.setString("ASColorGradeLUTFile", control->getValue().asString());
    });
    const LLHandle<LLPanel> handle = panel.getHandle();
    panel.getChild<LLButton>("lut_previous")->setCommitCallback([handle](LLUICtrl*, const LLSD&)
    {
        if (LLPanel* view = handle.get())
        {
            LLComboBox* combo = view->getChild<LLComboBox>("lut_file");
            if (combo->selectPrevItem()) gSavedSettings.setString("ASColorGradeLUTFile", combo->getValue().asString());
        }
    });
    panel.getChild<LLButton>("lut_next")->setCommitCallback([handle](LLUICtrl*, const LLSD&)
    {
        if (LLPanel* view = handle.get())
        {
            LLComboBox* combo = view->getChild<LLComboBox>("lut_file");
            if (combo->selectNextItem()) gSavedSettings.setString("ASColorGradeLUTFile", combo->getValue().asString());
        }
    });
    panel.getChild<LLButton>("lut_refresh")->setCommitCallback([handle](LLUICtrl*, const LLSD&)
    {
        sDirty = true;
        if (LLPanel* view = handle.get()) refreshPanel(*view);
    });
    panel.getChild<LLButton>("lut_import")->setCommitCallback([handle](LLUICtrl*, const LLSD&)
    {
        // The picker outlives a closed panel; only touch UI through a weak handle.
        LLFilePickerReplyThread::startPicker(
            [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
            {
                importFile(files);
                if (LLPanel* view = handle.get()) refreshPanel(*view);
            }, LLFilePicker::FFLOAD_ALL, false);
    });
    panel.getChild<LLButton>("lut_open_folder")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLFile::mkdir(userDir());
        if (LLFile::isdir(userDir())) gViewerWindow->getWindow()->openFile(userDir());
        else report("LUT folder", "Cannot create the user LUT folder.");
    });
    refreshPanel(panel);
}
