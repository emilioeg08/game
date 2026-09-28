#include "Tests/TestFramework.h"

#include "Engine/Serialization/Binary.h"
#include "Engine/Text/Localization.h"

#include <string>
#include <vector>

using namespace gx;

GX_TEST(Text, FormatPatternFillsAndNeverFails) {
    const std::vector<std::string> args = {"Faro-12", "Arenmir II", "20"};
    GX_EXPECT(formatPattern("{} atraca en {}.", args) == "Faro-12 atraca en Arenmir II.");
    GX_EXPECT(formatPattern("{1}: {0} ({2} t)", args) == "Arenmir II: Faro-12 (20 t)");
    GX_EXPECT(formatPattern("{{literal}} {}", args) == "{literal} Faro-12");
    // Malformed or missing arguments are copied, never an exception.
    GX_EXPECT(formatPattern("{} {} {} {}", args) == "Faro-12 Arenmir II 20 {}");
    GX_EXPECT(formatPattern("{7} {x} {", args) == "{7} {x} {");
}

GX_TEST(Text, PlaceholdersMustMatchBetweenSourceAndTranslation) {
    GX_EXPECT(compatiblePatterns("Créditos: %lld cr", "Credits: %lld cr"));
    GX_EXPECT(!compatiblePatterns("Créditos: %lld cr", "Credits: %s cr"));
    GX_EXPECT(!compatiblePatterns("%s en %s", "%s"));
    GX_EXPECT(compatiblePatterns("100 %% de %s", "%s at 100 %%"));
    GX_EXPECT(compatiblePatterns("{} atraca en {}.", "{} docks at {}."));
    GX_EXPECT(compatiblePatterns("{} atraca en {}.", "At {1}, {0} docks."));
    GX_EXPECT(!compatiblePatterns("{} atraca en {}.", "{} docks."));
    GX_EXPECT(!compatiblePatterns("{:.0f} cr/h", "{} cr/h"));
    GX_EXPECT(!compatiblePatterns("{} y {}", "{0} and {}")); // mixed numbering
    GX_EXPECT(compatiblePatterns("Agua", "Water"));
}

GX_TEST(Text, PoCatalogLoadsAndRejectsBadTranslations) {
    const std::string po = "\xEF\xBB\xBF# English\n"
                           "msgid \"\"\n"
                           "msgstr \"\"\n"
                           "\"Language: en\\n\"\n"
                           "\"X-Language-Name: English\\n\"\n"
                           "\n"
                           "#: Apps/Game/Menus.cpp\n"
                           "msgid \"Continuar\"\n"
                           "msgstr \"Continue\"\n"
                           "\n"
                           "msgid \"\"\n"
                           "\"{} atraca \"\n"
                           "\"en {}.\"\n"
                           "msgstr \"{} docks at {}.\"\n"
                           "\n"
                           "msgid \"Dijo \\\"hola\\\"\\n\"\n"
                           "msgstr \"Said \\\"hello\\\"\\n\"\n"
                           "\n"
                           "msgid \"Sin traducir\"\n"
                           "msgstr \"\"\n"
                           "\n"
                           "#, fuzzy\n"
                           "msgid \"Dudosa\"\n"
                           "msgstr \"Doubtful\"\n"
                           "\n"
                           "msgid \"Créditos: %lld cr\"\n"
                           "msgstr \"Credits: %s cr\"\n";
    Catalog catalog;
    std::string error;
    GX_REQUIRE(catalog.loadPo(po, error));
    GX_EXPECT(catalog.language() == "en");
    GX_EXPECT(catalog.languageName() == "English");
    GX_EXPECT_EQ(catalog.size(), 3u);
    GX_EXPECT(std::string(catalog.translate("Continuar")) == "Continue");
    GX_EXPECT(catalog.translate(std::string_view("{} atraca en {}.")) == "{} docks at {}.");
    GX_EXPECT(catalog.translate(std::string_view("Dijo \"hola\"\n")) == "Said \"hello\"\n");
    GX_EXPECT(std::string(catalog.translate("Sin traducir")) == "Sin traducir"); // falls back to the source
    GX_EXPECT(std::string(catalog.translate("Dudosa")) == "Dudosa");
    GX_REQUIRE(catalog.rejected().size() == 1u); // would have crashed printf
    GX_EXPECT(std::string(catalog.translate("Créditos: %lld cr")) == "Créditos: %lld cr");

    Catalog broken;
    GX_EXPECT(!broken.loadPo("msgid \"a\"\nmsgstr \"b\nx\"\n", error));
    GX_EXPECT(error.find("line 2") != std::string::npos);
}

GX_TEST(Text, MessagesRenderInTheActiveLanguageAndSurviveSaving) {
    Catalog english;
    std::string error;
    GX_REQUIRE(english.loadPo("msgid \"Noticias: {} vende {} t de {}.\"\n"
                              "msgstr \"News: {0} sells {1} t of {2}.\"\n"
                              "msgid \"Agua\"\nmsgstr \"Water\"\n"
                              "msgid \"Estación\"\nmsgstr \"Station\"\n",
                              error));
    const Message message =
        msg("Noticias: {} vende {} t de {}.", properName("Estación Arenmir IV"), number(20), term("Agua"));
    GX_EXPECT(render(message, nullptr) == "Noticias: Estación Arenmir IV vende 20 t de Agua.");
    GX_EXPECT(render(message, &english) == "News: Station Arenmir IV sells 20 t of Water.");
    GX_EXPECT(render(properName("Arenmir IV"), &english) == "Arenmir IV");
    GX_EXPECT(render(number(1.25, 1), nullptr) == "1.2" || render(number(1.25, 1), nullptr) == "1.3");

    std::vector<std::string> sources;
    collectSources(message, sources);
    GX_EXPECT(sources == (std::vector<std::string>{"Noticias: {} vende {} t de {}.", "Agua"}));

    BinaryWriter writer;
    writer.io(message);
    BinaryReader reader(writer.bytes());
    Message loaded;
    reader.io(loaded);
    GX_REQUIRE(reader.ok());
    GX_EXPECT(render(loaded, &english) == render(message, &english));

    setActiveCatalog(&english);
    GX_EXPECT(std::string(tr("Agua")) == "Water");
    GX_EXPECT(trf("Noticias: {} vende {} t de {}.", "A", 3, "B") == "News: A sells 3 t of B.");
    setActiveCatalog(nullptr);
    GX_EXPECT(std::string(tr("Agua")) == "Agua");
}
