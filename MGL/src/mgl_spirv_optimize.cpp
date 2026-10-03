#include "mgl_spirv_optimize.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <vector>

static_assert(sizeof(unsigned) == sizeof(uint32_t),
              "SPIR-V word ABI requires 32-bit unsigned");

namespace {

void reportOptimizerMessage(spv_message_level_t level,
                            const char *source,
                            const spv_position_t &position,
                            const char *message)
{
    const char *levelName = "info";
    switch (level) {
        case SPV_MSG_FATAL: levelName = "fatal"; break;
        case SPV_MSG_INTERNAL_ERROR: levelName = "internal-error"; break;
        case SPV_MSG_ERROR: levelName = "error"; break;
        case SPV_MSG_WARNING: levelName = "warning"; break;
        case SPV_MSG_INFO: levelName = "info"; break;
        case SPV_MSG_DEBUG: levelName = "debug"; break;
    }

    std::fprintf(stderr,
                 "MGL SPIR-V optimizer %s:%zu:%zu [%s]: %s\n",
                 source ? source : "<module>",
                 position.line,
                 position.column,
                 levelName,
                 message ? message : "(no detail)");
}

void reportFailure(const char *reason, size_t wordCount)
{
    std::fprintf(stderr,
                 "MGL SPIR-V optimizer failed: %s (environment=OpenGL 4.5, words=%zu)\n",
                 reason,
                 wordCount);
}

} // namespace

extern "C" bool mglOptimizeSPIRVForPerformance(unsigned **words,
                                                size_t *word_count)
{
    if (!words || !word_count || !*words || *word_count == 0u) {
        reportFailure("invalid input", word_count ? *word_count : 0u);
        return false;
    }

    if (*word_count > static_cast<size_t>(-1) / sizeof(unsigned)) {
        reportFailure("input word count overflows byte size", *word_count);
        return false;
    }

    const size_t inputWordCount = *word_count;
    try {
        spvtools::Optimizer optimizer(SPV_ENV_OPENGL_4_5);
        optimizer.SetMessageConsumer(reportOptimizerMessage);
        optimizer.RegisterPerformancePasses(true);

        spvtools::OptimizerOptions options;
        options.set_preserve_bindings(true);
        options.set_preserve_spec_constants(true);
        options.set_run_validator(true);

        std::vector<uint32_t> optimizedWords;
        if (!optimizer.Run(reinterpret_cast<const uint32_t *>(*words),
                           inputWordCount,
                           &optimizedWords,
                           options)) {
            reportFailure("SPIRV-Tools Run returned false", inputWordCount);
            return false;
        }
        if (optimizedWords.empty() ||
            optimizedWords.size() > static_cast<size_t>(-1) / sizeof(unsigned)) {
            reportFailure("optimizer produced an invalid output size", inputWordCount);
            return false;
        }

        // Run's validator checks its input. Validate the transformed module
        // too, before GL reflection or replacing the caller's owned storage.
        spvtools::SpirvTools validator(SPV_ENV_OPENGL_4_5);
        validator.SetMessageConsumer(reportOptimizerMessage);
        if (!validator.Validate(optimizedWords.data(), optimizedWords.size())) {
            reportFailure("optimized module failed validation", inputWordCount);
            return false;
        }

        const size_t outputBytes = optimizedWords.size() * sizeof(unsigned);
        unsigned *replacement = static_cast<unsigned *>(std::malloc(outputBytes));
        if (!replacement) {
            reportFailure("allocation of optimized module failed", inputWordCount);
            return false;
        }
        std::memcpy(replacement, optimizedWords.data(), outputBytes);

        std::free(*words);
        *words = replacement;
        *word_count = optimizedWords.size();
        return true;
    } catch (const std::bad_alloc &) {
        reportFailure("out of memory", inputWordCount);
        return false;
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "MGL SPIR-V optimizer exception: %s\n",
                     exception.what());
        return false;
    } catch (...) {
        reportFailure("unknown exception", inputWordCount);
        return false;
    }
}
