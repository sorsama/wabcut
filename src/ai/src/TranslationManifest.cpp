// Generated from the pinned OPUS-MT revisions (AI-12, docs/ai.md "Going global"): Helsinki-NLP's OPUS-MT models in
// Xenova's 8-bit ONNX exports, every file pinned to a repository revision with its size and SHA-256. Checked on
// 2026-10-12: the licences are the base models' (Apache-2.0, or CC-BY-4.0 with the attribution in the card); no NC.

#include "TranslationManifest.h"

namespace wabcut::ai {

std::vector<ModelInfo> translationManifest() {
    std::vector<ModelInfo> out;
    const auto add = [&](const char* id, const char* file, const char* url, std::uint64_t size, const char* sha, const char* license,
                         const char* card, const char* description) {
        out.push_back(ModelInfo{id, ModelKind::Translation, file, url, size, sha, license, card, description});
    };
    // Helsinki-NLP/opus-mt-en-mul -> Xenova/opus-mt-en-mul@e57f9759f8e1e9ed97067bfc11e53e1eab63e57e: English to many languages (Thai, Japanese, Vietnamese, Indonesian, ...), Apache-2.0
    add("opus-mt-en-mul-encoder", "opus-mt-en-mul/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-mul/resolve/e57f9759f8e1e9ed97067bfc11e53e1eab63e57e/onnx/encoder_model_quantized.onnx",
        52443550, "f10f49c2691043ba7cfd2a85d86f84dddda428d4209cf9b495f6e114b313737a", "Apache-2.0",
        "OPUS-MT en-mul (Helsinki-NLP/opus-mt-en-mul, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-mul",
        "OPUS-MT en-mul, English to many languages (Thai, Japanese, Vietnamese, Indonesian, ...) (118 MB in all)");
    add("opus-mt-en-mul-decoder", "opus-mt-en-mul/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-mul/resolve/e57f9759f8e1e9ed97067bfc11e53e1eab63e57e/onnx/decoder_model_quantized.onnx",
        59382346, "12c04e64038937e2b51483e9b8cb378ee5be5bad0dbf206097ebe01e507ecc34", "Apache-2.0",
        "OPUS-MT en-mul (Helsinki-NLP/opus-mt-en-mul, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-mul",
        "OPUS-MT en-mul decoder");
    add("opus-mt-en-mul-tokenizer", "opus-mt-en-mul/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-en-mul/resolve/e57f9759f8e1e9ed97067bfc11e53e1eab63e57e/tokenizer.json",
        6081943, "988af0af5cab7758c7548320a8b76c6a264d1ddfeac61e3f08d38d04d7202d6e", "Apache-2.0",
        "OPUS-MT en-mul (Helsinki-NLP/opus-mt-en-mul, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-mul",
        "OPUS-MT en-mul tokenizer");
    add("opus-mt-en-mul-config", "opus-mt-en-mul/config.json",
        "https://huggingface.co/Xenova/opus-mt-en-mul/resolve/e57f9759f8e1e9ed97067bfc11e53e1eab63e57e/config.json",
        1390, "fc12f9f431b4fb8dae1f7776c7ad3f9a5cf0fd6321231e11b45e35a2b11730f3", "Apache-2.0",
        "OPUS-MT en-mul (Helsinki-NLP/opus-mt-en-mul, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-mul",
        "OPUS-MT en-mul config");
    // Helsinki-NLP/opus-mt-th-en -> Xenova/opus-mt-th-en@7f0406b972f742985cfbe3be8f61dead9f1bc0e3: Thai to English, Apache-2.0
    add("opus-mt-th-en-encoder", "opus-mt-th-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-th-en/resolve/7f0406b972f742985cfbe3be8f61dead9f1bc0e3/onnx/encoder_model_quantized.onnx",
        51520414, "37e31edabcadfca300cf13d4dd20087aa1a920cba7b20c738cba8ff44f320bbf", "Apache-2.0",
        "OPUS-MT th-en (Helsinki-NLP/opus-mt-th-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-th-en",
        "OPUS-MT th-en, Thai to English (117 MB in all)");
    add("opus-mt-th-en-decoder", "opus-mt-th-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-th-en/resolve/7f0406b972f742985cfbe3be8f61dead9f1bc0e3/onnx/decoder_model_quantized.onnx",
        58451998, "15a61bb72df5b3f938469d7ba867909e19bf235bba6195b3811ce12c7bebbcc1", "Apache-2.0",
        "OPUS-MT th-en (Helsinki-NLP/opus-mt-th-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-th-en",
        "OPUS-MT th-en decoder");
    add("opus-mt-th-en-tokenizer", "opus-mt-th-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-th-en/resolve/7f0406b972f742985cfbe3be8f61dead9f1bc0e3/tokenizer.json",
        7011245, "5b6f580a27eb8ceb92f1a4d515b50d7d55a7a191f9cf66e936ce6ad2c149a38e", "Apache-2.0",
        "OPUS-MT th-en (Helsinki-NLP/opus-mt-th-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-th-en",
        "OPUS-MT th-en tokenizer");
    add("opus-mt-th-en-config", "opus-mt-th-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-th-en/resolve/7f0406b972f742985cfbe3be8f61dead9f1bc0e3/config.json",
        1389, "5a01981e5de74e636f045e55705dbf83a859c82599cf0d6e8ebbe271485ddf5b", "Apache-2.0",
        "OPUS-MT th-en (Helsinki-NLP/opus-mt-th-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-th-en",
        "OPUS-MT th-en config");
    // Helsinki-NLP/opus-mt-en-es -> Xenova/opus-mt-en-es@4b002a4c7edd54a7ced58877258b87f7efd3f892: English to Spanish, Apache-2.0
    add("opus-mt-en-es-encoder", "opus-mt-en-es/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/encoder_model_quantized.onnx",
        52899742, "03434edd1147e6c49e22066e9dc5888235e99cc35a6711d1c4ea5b06455d5da9", "Apache-2.0",
        "OPUS-MT en-es (Helsinki-NLP/opus-mt-en-es, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-es",
        "OPUS-MT en-es, English to Spanish (119 MB in all)");
    add("opus-mt-en-es-decoder", "opus-mt-en-es/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/onnx/decoder_model_quantized.onnx",
        59842102, "2b45074cc819f1d901e40510d7bc0781d99327d16c7eca04518c4ee0c3d6ba10", "Apache-2.0",
        "OPUS-MT en-es (Helsinki-NLP/opus-mt-en-es, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-es",
        "OPUS-MT en-es decoder");
    add("opus-mt-en-es-tokenizer", "opus-mt-en-es/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/tokenizer.json",
        6262682, "285eb29e7155ee48851a77960797813f86a125f70d2c1a124f613f1fbd2b19c3", "Apache-2.0",
        "OPUS-MT en-es (Helsinki-NLP/opus-mt-en-es, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-es",
        "OPUS-MT en-es tokenizer");
    add("opus-mt-en-es-config", "opus-mt-en-es/config.json",
        "https://huggingface.co/Xenova/opus-mt-en-es/resolve/4b002a4c7edd54a7ced58877258b87f7efd3f892/config.json",
        1468, "499eae1f97eeba63172fc884f92fd727ae803b7ceb79c0f653aa4b011152af2f", "Apache-2.0",
        "OPUS-MT en-es (Helsinki-NLP/opus-mt-en-es, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-es",
        "OPUS-MT en-es config");
    // Helsinki-NLP/opus-mt-en-fr -> Xenova/opus-mt-en-fr@28726206f80896b90035bd99cccd5cc1e151f916: English to French, Apache-2.0
    add("opus-mt-en-fr-encoder", "opus-mt-en-fr/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-fr/resolve/28726206f80896b90035bd99cccd5cc1e151f916/onnx/encoder_model_quantized.onnx",
        50090398, "0a81bdba62f53223740a8c6c6f58e716eba8ea1f92dfa569caf27ed5e3b6a0f7", "Apache-2.0",
        "OPUS-MT en-fr (Helsinki-NLP/opus-mt-en-fr, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-fr",
        "OPUS-MT en-fr, English to French (113 MB in all)");
    add("opus-mt-en-fr-decoder", "opus-mt-en-fr/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-fr/resolve/28726206f80896b90035bd99cccd5cc1e151f916/onnx/decoder_model_quantized.onnx",
        57010810, "bdacc5f38e4d2b805a2fc9f38cf43844cdea56666e735137dbacf2a54a785257", "Apache-2.0",
        "OPUS-MT en-fr (Helsinki-NLP/opus-mt-en-fr, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-fr",
        "OPUS-MT en-fr decoder");
    add("opus-mt-en-fr-tokenizer", "opus-mt-en-fr/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-en-fr/resolve/28726206f80896b90035bd99cccd5cc1e151f916/tokenizer.json",
        5637839, "8391785c1a2139e7af4678571ccd8dc654ecbb72e4be186940f65d7c604f0246", "Apache-2.0",
        "OPUS-MT en-fr (Helsinki-NLP/opus-mt-en-fr, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-fr",
        "OPUS-MT en-fr tokenizer");
    add("opus-mt-en-fr-config", "opus-mt-en-fr/config.json",
        "https://huggingface.co/Xenova/opus-mt-en-fr/resolve/28726206f80896b90035bd99cccd5cc1e151f916/config.json",
        1411, "b522b73fcc86f77981349c1df2a2e041eed0dbcbea4acf2635019cf21d51ffc0", "Apache-2.0",
        "OPUS-MT en-fr (Helsinki-NLP/opus-mt-en-fr, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-fr",
        "OPUS-MT en-fr config");
    // Helsinki-NLP/opus-mt-en-zh -> Xenova/opus-mt-en-zh@046f55aec303cdee3e0318604406d4df20f1e8ea: English to Chinese, Apache-2.0
    add("opus-mt-en-zh-encoder", "opus-mt-en-zh/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-zh/resolve/046f55aec303cdee3e0318604406d4df20f1e8ea/onnx/encoder_model_quantized.onnx",
        52899742, "d3b7912bf6a9bd27e4c074c2df91d4ff3d5b4bc5f7f6c8d7cc9c805c98fbafee", "Apache-2.0",
        "OPUS-MT en-zh (Helsinki-NLP/opus-mt-en-zh, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-zh",
        "OPUS-MT en-zh, English to Chinese (119 MB in all)");
    add("opus-mt-en-zh-decoder", "opus-mt-en-zh/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-zh/resolve/046f55aec303cdee3e0318604406d4df20f1e8ea/onnx/decoder_model_quantized.onnx",
        59842102, "2c66a3981099b40edbb3a0d65e015d05964d42fecb9780f8422776eff5939112", "Apache-2.0",
        "OPUS-MT en-zh (Helsinki-NLP/opus-mt-en-zh, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-zh",
        "OPUS-MT en-zh decoder");
    add("opus-mt-en-zh-tokenizer", "opus-mt-en-zh/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-en-zh/resolve/046f55aec303cdee3e0318604406d4df20f1e8ea/tokenizer.json",
        6380952, "d0c7da27056e8f42adce9e76d8e792e5daa64e15f5acd2e7aabf0121877dd4c1", "Apache-2.0",
        "OPUS-MT en-zh (Helsinki-NLP/opus-mt-en-zh, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-zh",
        "OPUS-MT en-zh tokenizer");
    add("opus-mt-en-zh-config", "opus-mt-en-zh/config.json",
        "https://huggingface.co/Xenova/opus-mt-en-zh/resolve/046f55aec303cdee3e0318604406d4df20f1e8ea/config.json",
        1503, "4727d1229a04f95bf6f39abf949d8080615433d99d6ebd85f81c09edd247d5fa", "Apache-2.0",
        "OPUS-MT en-zh (Helsinki-NLP/opus-mt-en-zh, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-zh",
        "OPUS-MT en-zh config");
    // Helsinki-NLP/opus-mt-en-de -> Xenova/opus-mt-en-de@1ca130c44c4c5441ef16d48aae521a424ab644f7: English to German, CC-BY-4.0
    add("opus-mt-en-de-encoder", "opus-mt-en-de/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-de/resolve/1ca130c44c4c5441ef16d48aae521a424ab644f7/onnx/encoder_model_quantized.onnx",
        49366942, "15834b45fabd2dfb8c6c029b3ca3e7289aeefd90ece798ce42bcf548d1bd3b8d", "CC-BY-4.0",
        "OPUS-MT en-de (Helsinki-NLP/opus-mt-en-de) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-de",
        "OPUS-MT en-de, English to German (111 MB in all)");
    add("opus-mt-en-de-decoder", "opus-mt-en-de/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-en-de/resolve/1ca130c44c4c5441ef16d48aae521a424ab644f7/onnx/decoder_model_quantized.onnx",
        56281702, "75ef79aa9bde9e3dce9ca584c29507be5f464973f6c600c89ff419bc8de29ebc", "CC-BY-4.0",
        "OPUS-MT en-de (Helsinki-NLP/opus-mt-en-de) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-de",
        "OPUS-MT en-de decoder");
    add("opus-mt-en-de-tokenizer", "opus-mt-en-de/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-en-de/resolve/1ca130c44c4c5441ef16d48aae521a424ab644f7/tokenizer.json",
        5498450, "8e0fcf45621ea87fa680c7f9969c37a7f819c1f4c7658a2e6e0879b866a14b17", "CC-BY-4.0",
        "OPUS-MT en-de (Helsinki-NLP/opus-mt-en-de) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-de",
        "OPUS-MT en-de tokenizer");
    add("opus-mt-en-de-config", "opus-mt-en-de/config.json",
        "https://huggingface.co/Xenova/opus-mt-en-de/resolve/1ca130c44c4c5441ef16d48aae521a424ab644f7/config.json",
        1411, "517955ebf66cd6523f9982851dd41771b74fb2d9c00b4933ba2922ac7a63336f", "CC-BY-4.0",
        "OPUS-MT en-de (Helsinki-NLP/opus-mt-en-de) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-en-de",
        "OPUS-MT en-de config");
    // Helsinki-NLP/opus-mt-ja-en -> Xenova/opus-mt-ja-en@1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16: Japanese to English, Apache-2.0
    add("opus-mt-ja-en-encoder", "opus-mt-ja-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-ja-en/resolve/1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16/onnx/encoder_model_quantized.onnx",
        50705822, "345262b16bcdda1468b0f3380c112b7ce79f731176b4b1d21f6edd5b2ae0d25c", "Apache-2.0",
        "OPUS-MT ja-en (Helsinki-NLP/opus-mt-ja-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ja-en",
        "OPUS-MT ja-en, Japanese to English (114 MB in all)");
    add("opus-mt-ja-en-decoder", "opus-mt-ja-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-ja-en/resolve/1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16/onnx/decoder_model_quantized.onnx",
        57631042, "ec639efdbb8dd9a5c458ab3aeeaee34402b21200eb9c24a95a32a16f0bb22d66", "Apache-2.0",
        "OPUS-MT ja-en (Helsinki-NLP/opus-mt-ja-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ja-en",
        "OPUS-MT ja-en decoder");
    add("opus-mt-ja-en-tokenizer", "opus-mt-ja-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-ja-en/resolve/1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16/tokenizer.json",
        5991485, "770ff2855437cf44f1f110550c5a9dca773253a167aeac36076b2073d259aa3b", "Apache-2.0",
        "OPUS-MT ja-en (Helsinki-NLP/opus-mt-ja-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ja-en",
        "OPUS-MT ja-en tokenizer");
    add("opus-mt-ja-en-config", "opus-mt-ja-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-ja-en/resolve/1a906cfaaf7c8f4193f67f5885c082aa6dbd9d16/config.json",
        1376, "d35476f1d3858883feec67e5a63ddc387452a70fda9c8e26db99c71aa2ad6a34", "Apache-2.0",
        "OPUS-MT ja-en (Helsinki-NLP/opus-mt-ja-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ja-en",
        "OPUS-MT ja-en config");
    // Helsinki-NLP/opus-mt-ko-en -> Xenova/opus-mt-ko-en@d9fa1ac6008242100fecb4fff9b0a5917a1be81f: Korean to English, Apache-2.0
    add("opus-mt-ko-en-encoder", "opus-mt-ko-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-ko-en/resolve/d9fa1ac6008242100fecb4fff9b0a5917a1be81f/onnx/encoder_model_quantized.onnx",
        52899741, "210e1d2cb5b457cdeeffe288099cc9d1df9f13c35dc3d39fde4ba252af877380", "Apache-2.0",
        "OPUS-MT ko-en (Helsinki-NLP/opus-mt-ko-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ko-en",
        "OPUS-MT ko-en, Korean to English (119 MB in all)");
    add("opus-mt-ko-en-decoder", "opus-mt-ko-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-ko-en/resolve/d9fa1ac6008242100fecb4fff9b0a5917a1be81f/onnx/decoder_model_quantized.onnx",
        59842101, "0c961ea33a62ea84e73c9ec94c0737e65d06b5f73a7d0398b018fbbb7262d929", "Apache-2.0",
        "OPUS-MT ko-en (Helsinki-NLP/opus-mt-ko-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ko-en",
        "OPUS-MT ko-en decoder");
    add("opus-mt-ko-en-tokenizer", "opus-mt-ko-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-ko-en/resolve/d9fa1ac6008242100fecb4fff9b0a5917a1be81f/tokenizer.json",
        6487044, "147a41edd62b08c79be8cfe6d3102ad19d8a964ac061b4bd6840ac45d0112011", "Apache-2.0",
        "OPUS-MT ko-en (Helsinki-NLP/opus-mt-ko-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ko-en",
        "OPUS-MT ko-en tokenizer");
    add("opus-mt-ko-en-config", "opus-mt-ko-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-ko-en/resolve/d9fa1ac6008242100fecb4fff9b0a5917a1be81f/config.json",
        1389, "8260080db48141328ae198ede2b25483fa4a6f46dc2859784d6e21c188865e43", "Apache-2.0",
        "OPUS-MT ko-en (Helsinki-NLP/opus-mt-ko-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-ko-en",
        "OPUS-MT ko-en config");
    // Helsinki-NLP/opus-mt-zh-en -> Xenova/opus-mt-zh-en@39d480d52a9ea3065a1f117adfe4dbc55de10e6f: Chinese to English, CC-BY-4.0
    add("opus-mt-zh-en-encoder", "opus-mt-zh-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-zh-en/resolve/39d480d52a9ea3065a1f117adfe4dbc55de10e6f/onnx/encoder_model_quantized.onnx",
        52899742, "84d5e171b626bc8b6b220d022ac58696e9528c25deeacca62b5cbf4364547a99", "CC-BY-4.0",
        "OPUS-MT zh-en (Helsinki-NLP/opus-mt-zh-en) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-zh-en",
        "OPUS-MT zh-en, Chinese to English (119 MB in all)");
    add("opus-mt-zh-en-decoder", "opus-mt-zh-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-zh-en/resolve/39d480d52a9ea3065a1f117adfe4dbc55de10e6f/onnx/decoder_model_quantized.onnx",
        59842102, "debcc3054b9ff9aaed972bd6e251a459abe1e2f1402da1efa63163dc2f46ef73", "CC-BY-4.0",
        "OPUS-MT zh-en (Helsinki-NLP/opus-mt-zh-en) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-zh-en",
        "OPUS-MT zh-en decoder");
    add("opus-mt-zh-en-tokenizer", "opus-mt-zh-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-zh-en/resolve/39d480d52a9ea3065a1f117adfe4dbc55de10e6f/tokenizer.json",
        6381339, "b306d0301cf280bfd647d7067b5ade2a97b987e6d678df110703c002433643ff", "CC-BY-4.0",
        "OPUS-MT zh-en (Helsinki-NLP/opus-mt-zh-en) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-zh-en",
        "OPUS-MT zh-en tokenizer");
    add("opus-mt-zh-en-config", "opus-mt-zh-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-zh-en/resolve/39d480d52a9ea3065a1f117adfe4dbc55de10e6f/config.json",
        1389, "293d318fce41dbf04114eac45037bb88a32d7c4ee21011a75e24a8b98ca45ad1", "CC-BY-4.0",
        "OPUS-MT zh-en (Helsinki-NLP/opus-mt-zh-en) by the Language Technology Research Group at the University of Helsinki (J. Tiedemann and S. Thottingal, OPUS-MT, EAMT 2020), licensed under Creative Commons Attribution 4.0 (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/); 8-bit ONNX export by Xenova, unchanged. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-zh-en",
        "OPUS-MT zh-en config");
    // Helsinki-NLP/opus-mt-es-en -> Xenova/opus-mt-es-en@eadfd7c658a9d8929ac3b8e996b68a68e2c7d480: Spanish to English, Apache-2.0
    add("opus-mt-es-en-encoder", "opus-mt-es-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/encoder_model_quantized.onnx",
        52899742, "c01e70f8455efc350831aa2af7ed187b16829241f0018ba07d1ba643a391bc18", "Apache-2.0",
        "OPUS-MT es-en (Helsinki-NLP/opus-mt-es-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-es-en",
        "OPUS-MT es-en, Spanish to English (119 MB in all)");
    add("opus-mt-es-en-decoder", "opus-mt-es-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/onnx/decoder_model_quantized.onnx",
        59842102, "d518f1f0041b2831f0aeb5a087cf899742cd9d4b574f6b98ad9f816b8c06925b", "Apache-2.0",
        "OPUS-MT es-en (Helsinki-NLP/opus-mt-es-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-es-en",
        "OPUS-MT es-en decoder");
    add("opus-mt-es-en-tokenizer", "opus-mt-es-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/tokenizer.json",
        6262682, "285eb29e7155ee48851a77960797813f86a125f70d2c1a124f613f1fbd2b19c3", "Apache-2.0",
        "OPUS-MT es-en (Helsinki-NLP/opus-mt-es-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-es-en",
        "OPUS-MT es-en tokenizer");
    add("opus-mt-es-en-config", "opus-mt-es-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-es-en/resolve/eadfd7c658a9d8929ac3b8e996b68a68e2c7d480/config.json",
        1433, "fab3a7f93185bc5aa7b419f6a1e6e74d98c8f2a506c94493d3019bf46da3478d", "Apache-2.0",
        "OPUS-MT es-en (Helsinki-NLP/opus-mt-es-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-es-en",
        "OPUS-MT es-en config");
    // Helsinki-NLP/opus-mt-fr-en -> Xenova/opus-mt-fr-en@6b166a182780e118c997879d0ad5be4b53671644: French to English, Apache-2.0
    add("opus-mt-fr-en-encoder", "opus-mt-fr-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-fr-en/resolve/6b166a182780e118c997879d0ad5be4b53671644/onnx/encoder_model_quantized.onnx",
        50090398, "e727cb26ac6bf816394c49671af69c9ae5798868fed1e40849872415dffc1772", "Apache-2.0",
        "OPUS-MT fr-en (Helsinki-NLP/opus-mt-fr-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-fr-en",
        "OPUS-MT fr-en, French to English (113 MB in all)");
    add("opus-mt-fr-en-decoder", "opus-mt-fr-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-fr-en/resolve/6b166a182780e118c997879d0ad5be4b53671644/onnx/decoder_model_quantized.onnx",
        57010810, "8c5a0c5d113a1c4c5ae46e014aacf770f3bff2109fe8b74ead6aae5c5f408ffc", "Apache-2.0",
        "OPUS-MT fr-en (Helsinki-NLP/opus-mt-fr-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-fr-en",
        "OPUS-MT fr-en decoder");
    add("opus-mt-fr-en-tokenizer", "opus-mt-fr-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-fr-en/resolve/6b166a182780e118c997879d0ad5be4b53671644/tokenizer.json",
        5637839, "8391785c1a2139e7af4678571ccd8dc654ecbb72e4be186940f65d7c604f0246", "Apache-2.0",
        "OPUS-MT fr-en (Helsinki-NLP/opus-mt-fr-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-fr-en",
        "OPUS-MT fr-en tokenizer");
    add("opus-mt-fr-en-config", "opus-mt-fr-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-fr-en/resolve/6b166a182780e118c997879d0ad5be4b53671644/config.json",
        1411, "6c2851f154d7b88c7767e4fdcf8a5694a36a456bfa6cbcde1b13fdbade3b56f6", "Apache-2.0",
        "OPUS-MT fr-en (Helsinki-NLP/opus-mt-fr-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-fr-en",
        "OPUS-MT fr-en config");
    // Helsinki-NLP/opus-mt-de-en -> Xenova/opus-mt-de-en@399dfd68706739fffd503f876093e455ae268a06: German to English, Apache-2.0
    add("opus-mt-de-en-encoder", "opus-mt-de-en/encoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-de-en/resolve/399dfd68706739fffd503f876093e455ae268a06/onnx/encoder_model_quantized.onnx",
        49366942, "4cedda8f8c89b72a42b3c6cd1e7a27f2de24457093e3bf80cb3e46829641fcd8", "Apache-2.0",
        "OPUS-MT de-en (Helsinki-NLP/opus-mt-de-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-de-en",
        "OPUS-MT de-en, German to English (111 MB in all)");
    add("opus-mt-de-en-decoder", "opus-mt-de-en/decoder_model_quantized.onnx",
        "https://huggingface.co/Xenova/opus-mt-de-en/resolve/399dfd68706739fffd503f876093e455ae268a06/onnx/decoder_model_quantized.onnx",
        56281702, "e44c1c4b50e8f51e49d4d5e54a9af1550dc74763d4023644a38af62611e6efc9", "Apache-2.0",
        "OPUS-MT de-en (Helsinki-NLP/opus-mt-de-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-de-en",
        "OPUS-MT de-en decoder");
    add("opus-mt-de-en-tokenizer", "opus-mt-de-en/tokenizer.json",
        "https://huggingface.co/Xenova/opus-mt-de-en/resolve/399dfd68706739fffd503f876093e455ae268a06/tokenizer.json",
        5498450, "8e0fcf45621ea87fa680c7f9969c37a7f819c1f4c7658a2e6e0879b866a14b17", "Apache-2.0",
        "OPUS-MT de-en (Helsinki-NLP/opus-mt-de-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-de-en",
        "OPUS-MT de-en tokenizer");
    add("opus-mt-de-en-config", "opus-mt-de-en/config.json",
        "https://huggingface.co/Xenova/opus-mt-de-en/resolve/399dfd68706739fffd503f876093e455ae268a06/config.json",
        1376, "6754743ec4754957d8552dad869d1314f3af6626b67446ec4cebe5e2fbaa0fc4", "Apache-2.0",
        "OPUS-MT de-en (Helsinki-NLP/opus-mt-de-en, Language Technology Research Group, University of Helsinki), Apache License 2.0; 8-bit ONNX export by Xenova. Translates captions on this computer: no caption text leaves it. https://huggingface.co/Helsinki-NLP/opus-mt-de-en",
        "OPUS-MT de-en config");
    return out;
}

} // namespace wabcut::ai
