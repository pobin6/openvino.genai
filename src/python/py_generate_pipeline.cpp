// Copyright (C) 2023-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>
#include "openvino/genai/continuous_batching_pipeline.hpp"
#include "openvino/genai/llm_pipeline.hpp"
#include <openvino/runtime/auto/properties.hpp>
#include "../cpp/src/tokenizers_path.hpp"

namespace py = pybind11;
using ov::genai::ChatHistory;
using ov::genai::ContinuousBatchingPipeline;
using ov::genai::DecodedResults;
using ov::genai::EncodedInputs;
using ov::genai::EncodedResults;
using ov::genai::GenerationConfig;
using ov::genai::GenerationResult;
using ov::genai::LLMPipeline;
using ov::genai::MeanStdPair;
using ov::genai::OptionalGenerationConfig;
using ov::genai::PerfMetrics;
using ov::genai::RawPerfMetrics;
using ov::genai::SchedulerConfig;
using ov::genai::StopCriteria;
using ov::genai::StreamerBase;
using ov::genai::StreamerVariant;
using ov::genai::StringInputs;
using ov::genai::TokenizedInputs;
using ov::genai::Tokenizer;

// When StreamerVariant is used utf-8 decoding is done by pybind and can lead to exception on incomplete texts.
// Therefore strings decoding should be handled with PyUnicode_DecodeUTF8(..., "replace") to not throw errors.
using PyBindStreamerVariant = std::variant<std::function<bool(py::str)>, std::shared_ptr<StreamerBase>, std::monostate>;

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

template <typename T, typename U>
std::vector<float> get_ms(const T& instance, U T::*member) {
    // Converts c++ duration to float so that it can be used in Python.
    std::vector<float> res;
    const auto& durations = instance.*member;
    res.reserve(durations.size());
    std::transform(durations.begin(), durations.end(), std::back_inserter(res),
                   [](const auto& duration) { return duration.count(); });
    return res;
}

namespace {

auto generate_docstring = R"(
    Generates sequences or tokens for LLMs. If input is a string or list of strings then resulting sequences will be already detokenized.

    :param inputs: inputs in the form of string, list of strings or tokenized input_ids
    :type inputs: str, List[str], ov.genai.TokenizedInputs, or ov.Tensor

    :param generation_config: generation_config
    :type generation_config: GenerationConfig or a Dict

    :param streamer: streamer either as a lambda with a boolean returning flag whether generation should be stopped
    :type : Callable[[str], bool], ov.genai.StreamerBase

    :param kwargs: arbitrary keyword arguments with keys corresponding to GenerationConfig fields.
    :type : Dict

    :return: return results in encoded, or decoded form depending on inputs type
    :rtype: DecodedResults, EncodedResults, str
)";

auto decoded_results_docstring = R"(
    Structure to store resulting batched text outputs and scores for each batch.
    The first num_return_sequences elements correspond to the first batch element.

    Parameters: 
    texts:      vector of resulting sequences.
    scores:     scores for each sequence.
    metrics:    performance metrics with tpot, ttft, etc. of type ov::genai::PerfMetrics.
)";

auto encoded_results_docstring = R"(
    Structure to store resulting batched tokens and scores for each batch sequence.
    The first num_return_sequences elements correspond to the first batch element.
    In the case if results decoded with beam search and random sampling scores contain
    sum of logarithmic probabilities for each token in the sequence. In the case
    of greedy decoding scores are filled with zeros.

    Parameters: 
    tokens: sequence of resulting tokens.
    scores: sum of logarithmic probabilities of all tokens in the sequence.
    metrics: performance metrics with tpot, ttft, etc. of type ov::genai::PerfMetrics.
)";

auto generation_config_docstring = R"(
    Structure to keep generation config parameters. For a selected method of decoding, only parameters from that group 
    and generic parameters are used. For example, if do_sample is set to true, then only generic parameters and random sampling parameters will 
    be used while greedy and beam search parameters will not affect decoding at all.

    Parameters: 
    max_length:    the maximum length the generated tokens can have. Corresponds to the length of the input prompt +
                   max_new_tokens. Its effect is overridden by `max_new_tokens`, if also set.
    max_new_tokens: the maximum numbers of tokens to generate, excluding the number of tokens in the prompt. max_new_tokens has priority over max_length.
    ignore_eos:    if set to true, then generation will not stop even if <eos> token is met.
    eos_token_id:  token_id of <eos> (end of sentence)
    min_new_tokens: set 0 probability for eos_token_id for the first eos_token_id generated tokens. Ignored for non continuous batching.
    stop_strings: list of strings that will cause pipeline to stop generating further tokens. Ignored for non continuous batching.
    include_stop_str_in_output: if set to true stop string that matched generation will be included in generation output (default: false)
    stop_token_ids: list of tokens that will cause pipeline to stop generating further tokens. Ignored for non continuous batching.

    Beam search specific parameters:
    num_beams:         number of beams for beam search. 1 disables beam search.
    num_beam_groups:   number of groups to divide `num_beams` into in order to ensure diversity among different groups of beams.
    diversity_penalty: value is subtracted from a beam's score if it generates the same token as any beam from other group at a particular time.
    length_penalty:    exponential penalty to the length that is used with beam-based generation. It is applied as an exponent to
        the sequence length, which in turn is used to divide the score of the sequence. Since the score is the log
        likelihood of the sequence (i.e. negative), length_penalty > 0.0 promotes longer sequences, while
        length_penalty < 0.0 encourages shorter sequences.
    num_return_sequences: the number of sequences to return for grouped beam search decoding.
    no_repeat_ngram_size: if set to int > 0, all ngrams of that size can only occur once.
    stop_criteria:        controls the stopping condition for grouped beam search. It accepts the following values: 
        "openvino_genai.StopCriteria.EARLY", where the generation stops as soon as there are `num_beams` complete candidates; 
        "openvino_genai.StopCriteria.HEURISTIC" is applied and the generation stops when is it very unlikely to find better candidates;
        "openvino_genai.StopCriteria.NEVER", where the beam search procedure only stops when there cannot be better candidates (canonical beam search algorithm).

    Random sampling parameters:
    temperature:        the value used to modulate token probabilities for random sampling.
    top_p:              if set to float < 1, only the smallest set of most probable tokens with probabilities that add up to top_p or higher are kept for generation.
    top_k:              the number of highest probability vocabulary tokens to keep for top-k-filtering.
    do_sample:          whether or not to use multinomial random sampling that add up to `top_p` or higher are kept.
    repetition_penalty: the parameter for repetition penalty. 1.0 means no penalty.    
)";

auto scheduler_config_docstring = R"(
    SchedulerConfig to construct ContinuousBatchingPipeline

    Parameters: 
    max_num_batched_tokens:     a maximum number of tokens to batch (in constrast to max_batch_size which combines
        independent sequences, we consider total amount of tokens in a batch).
    num_kv_blocks:              total number of KV blocks available to scheduler logic.
    cache_size:                 total size of KV cache in GB.
    block_size:                 block size for KV cache.
    dynamic_split_fuse:         whether to split prompt / generate to different scheduling phases.

    vLLM-like settings:
    max_num_seqs:               max number of scheduled sequences (you can think of it as "max batch size").
    enable_prefix_caching:      Enable caching of KV-blocks.
        When turned on all previously calculated KV-caches are kept in memory for future usages.
        KV-caches can be rewritten if KV-cache limit is reached, but blocks are not released.
        This results in more RAM usage, maximum RAM usage is determined by cache_size or num_kv_blocks parameters.
        When turend off only KV-cache required for batch calculation is kept in memory and
        when a sequence has finished genegartion its cache is released.
)";

auto generation_result_docstring = R"(
    GenerationResult stores resulting batched tokens and scores.

    Parameters: 
    request_id:         obsolete when handle API is approved as handle will connect results with prompts.
    generation_ids:     in a generic case we have multiple generation results per initial prompt
        depending on sampling parameters (e.g. beam search or parallel sampling).
    scores:             scores.
    status:             status of generation. The following values are possible:
        RUNNING = 0 - Default status for ongoing generation.
        FINISHED = 1 - Status set when generation has been finished.
        IGNORED = 2 - Status set when generation run into out-of-memory condition and could not be continued.
        DROPPED_BY_PIPELINE = 3 - Currently not used, TODO: implement abort functionality.
        DROPPED_BY_HANDLE = 4 - Status set when generation handle is dropped.

)";

auto stop_criteria_docstring =  R"(
    StopCriteria controls the stopping condition for grouped beam search.
    
    The following values are possible:
        "openvino_genai.StopCriteria.EARLY" stops as soon as there are `num_beams` complete candidates.
        "openvino_genai.StopCriteria.HEURISTIC" stops when is it unlikely to find better candidates.
        "openvino_genai.StopCriteria.NEVER" stops when there cannot be better candidates.
)";

auto streamer_base_docstring =  R"(
    Base class for streamers. In order to use inherit from from this class and inplement put, and methods.
)";

auto tokenized_inputs_docstring =  R"(
    Structure to agregate inputs to model.
    
    Parameters: 
    input_ids:         numerical token IDs from the tokenizer
    attention_mask:    indicates which tokens are attended to
)";

auto raw_perf_metrics_docstring = R"(
    Structure with raw performance metrics for each generation before any statistics are calculated.

    :param generate_durations: Durations for each generate call in microseconds.
    :type generate_durations: List[MicroSeconds]

    :param tokenization_durations: Durations for the tokenization process in microseconds.
    :type tokenization_durations: List[MicroSeconds]

    :param detokenization_durations: Durations for the detokenization process in microseconds.
    :type detokenization_durations: List[MicroSeconds]

    :param m_times_to_first_token: Times to the first token for each call in microseconds.
    :type m_times_to_first_token: List[MicroSeconds]

    :param m_new_token_times: Time points for each new token generated.
    :type m_new_token_times: List[TimePoint]

    :param m_batch_sizes: Batch sizes for each generate call.
    :type m_batch_sizes: List[int]

    :param m_durations: Total durations for each generate call in microseconds.
    :type m_durations: List[MicroSeconds]

    :param num_generated_tokens: Total number of tokens generated.
    :type num_generated_tokens: int

    :param num_input_tokens: Total number of tokens in the input prompt.
    :type num_input_tokens: int
)";

auto perf_metrics_docstring = R"(
    Holds performance metrics for each generate call.
    
    PerfMetrics holds fields with mean and standard deviations for the following metrics:
    - Time To the First Token (TTFT), ms
    - Time per Output Token (TPOT), ms/token
    - Generate total duration, ms
    - Tokenization duration, ms
    - Detokenization duration, ms
    - Throughput, tokens/s

    Additional fields include:
    - Load time, ms
    - Number of generated tokens
    - Number of tokens in the input prompt

    Preferable way to access values is via get functions. Getters calculate mean and std values from raw_metrics and return pairs.
    If mean and std were already calculated, getters return cached values.

    :param get_load_time: Returns the load time in milliseconds.
    :type get_load_time: float

    :param get_num_generated_tokens: Returns the number of generated tokens.
    :type get_num_generated_tokens: int

    :param get_num_input_tokens: Returns the number of tokens in the input prompt.
    :type get_num_input_tokens: int

    :param get_ttft: Returns the mean and standard deviation of TTFT.
    :type get_ttft: MeanStdPair

    :param get_tpot: Returns the mean and standard deviation of TPOT.
    :type get_tpot: MeanStdPair

    :param get_throughput: Returns the mean and standard deviation of throughput.
    :type get_throughput: MeanStdPair

    :param get_generate_duration: Returns the mean and standard deviation of generate duration.
    :type get_generate_duration: MeanStdPair

    :param get_tokenization_duration: Returns the mean and standard deviation of tokenization duration.
    :type get_tokenization_duration: MeanStdPair

    :param get_detokenization_duration: Returns the mean and standard deviation of detokenization duration.
    :type get_detokenization_duration: MeanStdPair

    :param raw_metrics: A structure of RawPerfMetrics type that holds raw metrics.
    :type raw_metrics: RawPerfMetrics
)";

OptionalGenerationConfig update_config_from_kwargs(const OptionalGenerationConfig& config, const py::kwargs& kwargs) {
    if(!config.has_value() && kwargs.empty())
        return std::nullopt;

    GenerationConfig res_config;
    if(config.has_value())
        res_config = *config;
 
    for (const auto& item : kwargs) {
        std::string key = py::cast<std::string>(item.first);
        py::object value = py::cast<py::object>(item.second);

        if (item.second.is_none()) {
            // Even if argument key name does not fit GenerationConfig name 
            // it's not an eror if it's not defined. 
            // Some HF configs can have parameters for methods currenly unsupported in ov_genai
            // but if their values are not set / None, then this should not block 
            // us from reading such configs, e.g. {"typical_p": None, 'top_p': 1.0,...}
            return res_config;
        }
        
        if (key == "max_new_tokens") {
            res_config.max_new_tokens = py::cast<int>(item.second);
        } else if (key == "max_length") {
            res_config.max_length = py::cast<int>(item.second);
        } else if (key == "ignore_eos") {
            res_config.ignore_eos = py::cast<bool>(item.second);
        } else if (key == "num_beam_groups") {
            res_config.num_beam_groups = py::cast<int>(item.second);
        } else if (key == "num_beams") {
            res_config.num_beams = py::cast<int>(item.second);
        } else if (key == "diversity_penalty") {
            res_config.diversity_penalty = py::cast<float>(item.second);
        } else if (key == "length_penalty") {
            res_config.length_penalty = py::cast<float>(item.second);
        } else if (key == "num_return_sequences") {
            res_config.num_return_sequences = py::cast<int>(item.second);
        } else if (key == "no_repeat_ngram_size") {
            res_config.no_repeat_ngram_size = py::cast<int>(item.second);
        } else if (key == "stop_criteria") {
            res_config.stop_criteria = py::cast<StopCriteria>(item.second);
        } else if (key == "temperature") {
            res_config.temperature = py::cast<float>(item.second);
        } else if (key == "top_p") {
            res_config.top_p = py::cast<float>(item.second);
        } else if (key == "top_k") {
            res_config.top_k = py::cast<int>(item.second);
        } else if (key == "do_sample") {
            res_config.do_sample = py::cast<bool>(item.second);
        } else if (key == "repetition_penalty") {
            res_config.repetition_penalty = py::cast<float>(item.second);
        } else if (key == "eos_token_id") {
            res_config.set_eos_token_id(py::cast<int>(item.second));
        } else {
            throw(std::invalid_argument("'" + key + "' is incorrect GenerationConfig parameter name. "
                                        "Use help(openvino_genai.GenerationConfig) to get list of acceptable parameters."));
        }
    }

    return res_config;
}

ov::Any py_object_to_any(const py::object& py_obj);

bool py_object_is_any_map(const py::object& py_obj) {
    if (!py::isinstance<py::dict>(py_obj)) {
        return false;
    }
    auto dict = py::cast<py::dict>(py_obj);
    return std::all_of(dict.begin(), dict.end(), [&](const std::pair<py::object::handle, py::object::handle>& elem) {
        return py::isinstance<py::str>(elem.first);
    });
}

ov::AnyMap py_object_to_any_map(const py::object& py_obj) {
    OPENVINO_ASSERT(py_object_is_any_map(py_obj), "Unsupported attribute type.");
    ov::AnyMap return_value = {};
    for (auto& item : py::cast<py::dict>(py_obj)) {
        std::string key = py::cast<std::string>(item.first);
        py::object value = py::cast<py::object>(item.second);
        if (py_object_is_any_map(value)) {
            return_value[key] = py_object_to_any_map(value);
        } else {
            return_value[key] = py_object_to_any(value);
        }
    }
    return return_value;
}

ov::Any py_object_to_any(const py::object& py_obj) {
    // Python types
    py::object float_32_type = py::module_::import("numpy").attr("float32");
    
    if (py::isinstance<py::str>(py_obj)) {
        return py_obj.cast<std::string>();
    } else if (py::isinstance<py::bool_>(py_obj)) {
        return py_obj.cast<bool>();
    } else if (py::isinstance<py::bytes>(py_obj)) {
        return py_obj.cast<std::string>();
    } else if (py::isinstance<py::float_>(py_obj)) {
        return py_obj.cast<double>();
    } else if (py::isinstance(py_obj, float_32_type)) {
        return py_obj.cast<float>();
    } else if (py::isinstance<py::int_>(py_obj)) {
        return py_obj.cast<int64_t>();
    } else if (py::isinstance<py::none>(py_obj)) {
        return {};
    } else if (py::isinstance<py::list>(py_obj)) {
        auto _list = py_obj.cast<py::list>();
        enum class PY_TYPE : int { UNKNOWN = 0, STR, INT, FLOAT, BOOL, PARTIAL_SHAPE };
        PY_TYPE detected_type = PY_TYPE::UNKNOWN;
        for (const auto& it : _list) {
            auto check_type = [&](PY_TYPE type) {
                if (detected_type == PY_TYPE::UNKNOWN || detected_type == type) {
                    detected_type = type;
                    return;
                }
                OPENVINO_THROW("Incorrect attribute. Mixed types in the list are not allowed.");
            };
            if (py::isinstance<py::str>(it)) {
                check_type(PY_TYPE::STR);
            } else if (py::isinstance<py::int_>(it)) {
                check_type(PY_TYPE::INT);
            } else if (py::isinstance<py::float_>(it)) {
                check_type(PY_TYPE::FLOAT);
            } else if (py::isinstance<py::bool_>(it)) {
                check_type(PY_TYPE::BOOL);
            } else if (py::isinstance<ov::PartialShape>(it)) {
                check_type(PY_TYPE::PARTIAL_SHAPE);
            }
        }

        if (_list.empty())
            return ov::Any();

        switch (detected_type) {
        case PY_TYPE::STR:
            return _list.cast<std::vector<std::string>>();
        case PY_TYPE::FLOAT:
            return _list.cast<std::vector<double>>();
        case PY_TYPE::INT:
            return _list.cast<std::vector<int64_t>>();
        case PY_TYPE::BOOL:
            return _list.cast<std::vector<bool>>();
        case PY_TYPE::PARTIAL_SHAPE:
            return _list.cast<std::vector<ov::PartialShape>>();
        default:
            OPENVINO_ASSERT(false, "Unsupported attribute type.");
        }
    
    // OV types
    } else if (py_object_is_any_map(py_obj)) {
        return py_object_to_any_map(py_obj);
    } else if (py::isinstance<ov::Any>(py_obj)) {
        return py::cast<ov::Any>(py_obj);
    } else if (py::isinstance<ov::element::Type>(py_obj)) {
        return py::cast<ov::element::Type>(py_obj);
    } else if (py::isinstance<ov::PartialShape>(py_obj)) {
        return py::cast<ov::PartialShape>(py_obj);
    } else if (py::isinstance<ov::hint::Priority>(py_obj)) {
        return py::cast<ov::hint::Priority>(py_obj);
    } else if (py::isinstance<ov::hint::PerformanceMode>(py_obj)) {
        return py::cast<ov::hint::PerformanceMode>(py_obj);
    } else if (py::isinstance<ov::intel_auto::SchedulePolicy>(py_obj)) {
        return py::cast<ov::intel_auto::SchedulePolicy>(py_obj);
    } else if (py::isinstance<ov::hint::SchedulingCoreType>(py_obj)) {
        return py::cast<ov::hint::SchedulingCoreType>(py_obj);
    } else if (py::isinstance<std::set<ov::hint::ModelDistributionPolicy>>(py_obj)) {
        return py::cast<std::set<ov::hint::ModelDistributionPolicy>>(py_obj);
    } else if (py::isinstance<ov::hint::ExecutionMode>(py_obj)) {
        return py::cast<ov::hint::ExecutionMode>(py_obj);
    } else if (py::isinstance<ov::log::Level>(py_obj)) {
        return py::cast<ov::log::Level>(py_obj);
    } else if (py::isinstance<ov::device::Type>(py_obj)) {
        return py::cast<ov::device::Type>(py_obj);
    } else if (py::isinstance<ov::streams::Num>(py_obj)) {
        return py::cast<ov::streams::Num>(py_obj);
    } else if (py::isinstance<ov::Affinity>(py_obj)) {
        return py::cast<ov::Affinity>(py_obj);
    } else if (py::isinstance<ov::Tensor>(py_obj)) {
        return py::cast<ov::Tensor>(py_obj);
    } else if (py::isinstance<ov::Output<ov::Node>>(py_obj)) {
        return py::cast<ov::Output<ov::Node>>(py_obj);
    } else if (py::isinstance<ov::genai::SchedulerConfig>(py_obj)) {
        return py::cast<ov::genai::SchedulerConfig>(py_obj);
    } else if (py::isinstance<py::object>(py_obj)) {
        return py_obj;
    }
    OPENVINO_ASSERT(false, "Unsupported attribute type.");
}

std::map<std::string, ov::Any> properties_to_any_map(const std::map<std::string, py::object>& properties) {
    std::map<std::string, ov::Any> properties_to_cpp;
    for (const auto& property : properties) {
        properties_to_cpp[property.first] = py_object_to_any(property.second);
    }
    return properties_to_cpp;
}

py::list handle_utf8_results(const std::vector<std::string>& decoded_res) {
    // pybind11 decodes strings similar to Pythons's
    // bytes.decode('utf-8'). It raises if the decoding fails.
    // generate() may return incomplete Unicode points if max_new_tokens
    // was reached. Replace such points with � instead of raising an exception
    py::list res;
    for (const auto s: decoded_res) {
        PyObject* py_s = PyUnicode_DecodeUTF8(s.data(), s.length(), "replace");
        res.append(py::reinterpret_steal<py::object>(py_s));
    }
    return res;
}

py::object call_common_generate(
    LLMPipeline& pipe, 
    const std::variant<ov::Tensor, TokenizedInputs, std::string, std::vector<std::string>>& inputs, 
    const OptionalGenerationConfig& config, 
    const PyBindStreamerVariant& py_streamer, 
    const py::kwargs& kwargs
) {
    auto updated_config = update_config_from_kwargs(config, kwargs);
    py::object results;
    EncodedInputs tensor_data;
    StreamerVariant streamer = std::monostate();
    
    std::visit(overloaded {
    [&streamer](const std::function<bool(py::str)>& py_callback){
        // Wrap python streamer with manual utf-8 decoding. Do not rely
        // on pybind automatic decoding since it raises exceptions on incomplete strings.
        auto callback_wrapped = [&py_callback](std::string subword) -> bool {
            auto py_str = PyUnicode_DecodeUTF8(subword.data(), subword.length(), "replace");
            return py_callback(py::reinterpret_borrow<py::str>(py_str));
        };
        streamer = callback_wrapped;
    },
    [&streamer](std::shared_ptr<StreamerBase> streamer_cls){
        streamer = streamer_cls;
    },
    [](std::monostate none){ /*streamer is already a monostate */ }
    }, py_streamer);

    // Call suitable generate overload for each type of input.
    std::visit(overloaded {
    [&](ov::Tensor ov_tensor) {
        results = py::cast(pipe.generate(ov_tensor, updated_config, streamer));
    },
    [&](TokenizedInputs tokenized_input) {
        results = py::cast(pipe.generate(tokenized_input, updated_config, streamer));
    },
    [&](std::string string_input) {
        DecodedResults res = pipe.generate(string_input, updated_config, streamer);
        // If input was a string return a single string otherwise return DecodedResults.
        if (updated_config.has_value() && (*updated_config).num_return_sequences == 1) {
            results = py::cast<py::object>(handle_utf8_results(res.texts)[0]);
        } else {
            results = py::cast(res);
        }
    },
    [&](std::vector<std::string> string_input) {
        // For DecodedResults texts getter already handles utf8 decoding.
        results = py::cast(pipe.generate(string_input, updated_config, streamer));
    }},
    inputs);
    
    return results;
}

std::string ov_tokenizers_module_path() {
    // Try a path relative to build artifacts folder first.
    std::filesystem::path from_relative = tokenizers_relative_to_genai();
    if (std::filesystem::exists(from_relative)) {
        return from_relative.string();
    }
    return py::str(py::module_::import("openvino_tokenizers").attr("_ext_path"));
}

class ConstructableStreamer: public StreamerBase {
    bool put(int64_t token) override {
        PYBIND11_OVERRIDE_PURE(
            bool,  // Return type
            StreamerBase,  // Parent class
            put,  // Name of function in C++ (must match Python name)
            token  // Argument(s)
        );
    }
    void end() override {
        PYBIND11_OVERRIDE_PURE(void, StreamerBase, end);
    }
};

std::ostream& operator << (std::ostream& stream, const GenerationResult& generation_result) {
    stream << generation_result.m_request_id << std::endl;
    const bool has_scores = !generation_result.m_scores.empty();
    for (size_t i = 0; i < generation_result.m_generation_ids.size(); ++i) {
        stream << "{ ";
        if (has_scores)
            stream << generation_result.m_scores[i] << ", ";
        stream << generation_result.m_generation_ids[i] << " }" << std::endl;
    }
    return stream << std::endl;
}
} // namespace


PYBIND11_MODULE(py_generate_pipeline, m) {
    m.doc() = "Pybind11 binding for LLM Pipeline";

    py::class_<LLMPipeline>(m, "LLMPipeline", "This class is used for generation with LLMs")
        .def(py::init([](
            const std::string& model_path, 
            const std::string& device,
            const std::map<std::string, py::object>& config
        ) {
            ScopedVar env_manager(ov_tokenizers_module_path());
            return std::make_unique<LLMPipeline>(model_path, device, properties_to_any_map(config));
        }),
        py::arg("model_path"), "folder with openvino_model.xml and openvino_tokenizer[detokenizer].xml files", 
        py::arg("device") = "CPU", "device on which inference will be done",
        py::arg("config") = ov::AnyMap({}), "openvino.properties map",
        R"(
            LLMPipeline class constructor.
            model_path (str): Path to the model file.
            device (str): Device to run the model on (e.g., CPU, GPU). Default is 'CPU'.
            Add {"scheduler_config": ov_genai.SchedulerConfig} to config properties to create continuous batching pipeline.
        )")

        .def(py::init([](
            const std::string& model_path,
            const Tokenizer& tokenizer,
            const std::string& device,
            const std::map<std::string, py::object>& config
        ) {
            ScopedVar env_manager(ov_tokenizers_module_path());
            return std::make_unique<LLMPipeline>(model_path, tokenizer, device, properties_to_any_map(config));
        }),
        py::arg("model_path"),
        py::arg("tokenizer"),
        py::arg("device") = "CPU",
        py::arg("config") = ov::AnyMap({}), "openvino.properties map",
        R"(
            LLMPipeline class constructor for manualy created openvino_genai.Tokenizer.
            model_path (str): Path to the model file.
            tokenizer (openvino_genai.Tokenizer): tokenizer object.
            device (str): Device to run the model on (e.g., CPU, GPU). Default is 'CPU'.
            Add {"scheduler_config": ov_genai.SchedulerConfig} to config properties to create continuous batching pipeline.
        )")

        .def(
            "generate", 
            [](LLMPipeline& pipe, 
                const std::variant<ov::Tensor, TokenizedInputs, std::string, std::vector<std::string>>& inputs, 
                const OptionalGenerationConfig& generation_config, 
                const PyBindStreamerVariant& streamer, 
                const py::kwargs& kwargs
            ) {
                return call_common_generate(pipe, inputs, generation_config, streamer, kwargs);
            },
            py::arg("inputs"), "Input string, or list of string or encoded tokens",
            py::arg("generation_config") = std::nullopt, "generation_config",
            py::arg("streamer") = std::monostate(), "streamer",
            (generate_docstring + std::string(" \n ") + generation_config_docstring).c_str()
        )

        .def(
            "__call__", 
            [](LLMPipeline& pipe, 
                const std::variant<ov::Tensor, TokenizedInputs, std::string, std::vector<std::string>>& inputs, 
                const OptionalGenerationConfig& generation_config, 
                const PyBindStreamerVariant& streamer, 
                const py::kwargs& kwargs
            ) {
                return call_common_generate(pipe, inputs, generation_config, streamer, kwargs);
            },
            py::arg("inputs"), "Input string, or list of string or encoded tokens",
            py::arg("generation_config") = std::nullopt, "generation_config",
            py::arg("streamer") = std::monostate(), "streamer",
            (generate_docstring + std::string(" \n ") + generation_config_docstring).c_str()
        )

        .def("get_tokenizer", &LLMPipeline::get_tokenizer)
        .def("start_chat", &LLMPipeline::start_chat, py::arg("system_message") = "")
        .def("finish_chat", &LLMPipeline::finish_chat)
        .def("get_generation_config", &LLMPipeline::get_generation_config, py::return_value_policy::copy)
        .def("set_generation_config", &LLMPipeline::set_generation_config);

     // Binding for Tokenizer
    py::class_<ov::genai::Tokenizer>(m, "Tokenizer",
        R"(openvino_genai.Tokenizer object is used to initialize Tokenizer 
           if it's located in a different path than the main model.)")
        
        .def(py::init([](const std::string& tokenizer_path, const std::map<std::string, py::object>& plugin_config) {
            ScopedVar env_manager(ov_tokenizers_module_path());
            return std::make_unique<ov::genai::Tokenizer>(tokenizer_path, properties_to_any_map(plugin_config));
        }), py::arg("tokenizer_path"), py::arg("plugin_config") = ov::AnyMap({}))
        
        .def("encode", [](Tokenizer& tok, std::vector<std::string>& prompts) { return tok.encode(prompts); },
            py::arg("prompts"),
            R"(Encodes a list of prompts into tokenized inputs.)")

        .def("encode", py::overload_cast<const std::string>(&Tokenizer::encode),
            py::arg("prompt"),
            R"(Encodes a single prompt into tokenized input.)")
        
        .def(
            "decode", 
            [](Tokenizer& tok, std::vector<int64_t>& tokens) -> py::str { 
                return handle_utf8_results({tok.decode(tokens)})[0];
            },
            py::arg("tokens"),
            R"(Decode a sequence into a string prompt.)"
        )
        
        .def(
            "decode", 
            [](Tokenizer& tok, ov::Tensor& tokens) -> py::list { 
                return handle_utf8_results(tok.decode(tokens)); 
            },
            py::arg("tokens"),
            R"(Decode tensor into a list of string prompts.)")
        
        .def(
            "decode", 
            [](Tokenizer& tok, std::vector<std::vector<int64_t>>& tokens) -> py::list{ 
                return handle_utf8_results(tok.decode(tokens)); 
            },
            py::arg("tokens"),
            R"(Decode a batch of tokens into a list of string prompt.)")
        
        .def("apply_chat_template", [](Tokenizer& tok,
                                        ChatHistory history,
                                        bool add_generation_prompt,
                                        const std::string& chat_template) {
            return tok.apply_chat_template(history, add_generation_prompt, chat_template);
        }, 
            py::arg("history"), 
            py::arg("add_generation_prompt"), 
            py::arg("chat_template") = "",
            R"(Embeds input prompts with special tags for a chat scenario.)")
        
        .def(
            "set_chat_template", &Tokenizer::set_chat_template,
            py::arg("chat_template"), "The new template to override with.",
            "Override a chat_template read from tokenizer_config.json."
        )

        .def("get_pad_token_id", &Tokenizer::get_pad_token_id)
        .def("get_bos_token_id", &Tokenizer::get_bos_token_id)
        .def("get_eos_token_id", &Tokenizer::get_eos_token_id)
        .def("get_pad_token", &Tokenizer::get_pad_token)
        .def("get_bos_token", &Tokenizer::get_bos_token)
        .def("get_eos_token", &Tokenizer::get_eos_token);

    // Binding for StopCriteria
    py::enum_<StopCriteria>(m, "StopCriteria", stop_criteria_docstring)
        .value("EARLY", StopCriteria::EARLY)
        .value("HEURISTIC", StopCriteria::HEURISTIC)
        .value("NEVER", StopCriteria::NEVER)
        .export_values();

     // Binding for GenerationConfig
    py::class_<GenerationConfig>(m, "GenerationConfig", generation_config_docstring)
        .def(py::init<std::string>(), py::arg("json_path"), "path where generation_config.json is stored")
        .def(py::init([](py::kwargs kwargs) { return *update_config_from_kwargs(GenerationConfig(), kwargs); }))
        .def_readwrite("max_new_tokens", &GenerationConfig::max_new_tokens)
        .def_readwrite("max_length", &GenerationConfig::max_length)
        .def_readwrite("ignore_eos", &GenerationConfig::ignore_eos)
        .def_readwrite("min_new_tokens", &GenerationConfig::min_new_tokens)
        .def_readwrite("num_beam_groups", &GenerationConfig::num_beam_groups)
        .def_readwrite("num_beams", &GenerationConfig::num_beams)
        .def_readwrite("diversity_penalty", &GenerationConfig::diversity_penalty)
        .def_readwrite("length_penalty", &GenerationConfig::length_penalty)
        .def_readwrite("num_return_sequences", &GenerationConfig::num_return_sequences)
        .def_readwrite("no_repeat_ngram_size", &GenerationConfig::no_repeat_ngram_size)
        .def_readwrite("stop_criteria", &GenerationConfig::stop_criteria)
        .def_readwrite("temperature", &GenerationConfig::temperature)
        .def_readwrite("top_p", &GenerationConfig::top_p)
        .def_readwrite("top_k", &GenerationConfig::top_k)
        .def_readwrite("do_sample", &GenerationConfig::do_sample)
        .def_readwrite("repetition_penalty", &GenerationConfig::repetition_penalty)
        .def_readwrite("eos_token_id", &GenerationConfig::eos_token_id)
        .def_readwrite("presence_penalty", &GenerationConfig::presence_penalty)
        .def_readwrite("frequency_penalty", &GenerationConfig::frequency_penalty)
        .def_readwrite("rng_seed", &GenerationConfig::rng_seed)
        .def_readwrite("stop_strings", &GenerationConfig::stop_strings)
        .def_readwrite("include_stop_str_in_output", &GenerationConfig::include_stop_str_in_output)
        .def_readwrite("stop_token_ids", &GenerationConfig::stop_token_ids)
        .def("set_eos_token_id", &GenerationConfig::set_eos_token_id)
        .def("is_beam_search", &GenerationConfig::is_beam_search);

    py::class_<DecodedResults>(m, "DecodedResults", decoded_results_docstring)
        .def(py::init<>())
        .def_property_readonly("texts", [](const DecodedResults &dr) { return handle_utf8_results(dr); })
        .def_readonly("scores", &DecodedResults::scores)
        .def_readonly("perf_metrics", &DecodedResults::perf_metrics)
        .def("__str__", &DecodedResults::operator std::string);

    py::class_<RawPerfMetrics>(m, "RawPerfMetrics", raw_perf_metrics_docstring)
        .def(py::init<>())
        .def_readonly("generate_durations", &RawPerfMetrics::generate_durations)
        .def_property_readonly("tokenization_durations", [](const RawPerfMetrics &rw) { 
            return get_ms(rw, &RawPerfMetrics::tokenization_durations);
         })
        .def_property_readonly("detokenization_durations", [](const RawPerfMetrics &rw) { 
            return get_ms(rw, &RawPerfMetrics::detokenization_durations); 
        })
        .def_property_readonly("m_times_to_first_token", [](const RawPerfMetrics &rw) { 
            return get_ms(rw, &RawPerfMetrics::m_times_to_first_token); 
        })
        .def_property_readonly("m_durations", [](const RawPerfMetrics &rw) { 
            return get_ms(rw, &RawPerfMetrics::m_durations); 
        })
        .def_readonly("m_batch_sizes", &RawPerfMetrics::m_batch_sizes)
        .def_readonly("num_generated_tokens", &RawPerfMetrics::num_generated_tokens)
        .def_readonly("num_input_tokens", &RawPerfMetrics::num_input_tokens);

    py::class_<MeanStdPair>(m, "MeanStdPair")
        .def(py::init<>())
        .def_readonly("mean", &MeanStdPair::mean)
        .def_readonly("std", &MeanStdPair::std);

    py::class_<PerfMetrics>(m, "PerfMetrics", perf_metrics_docstring)
        .def(py::init<>())
        .def("get_generate_duration", &PerfMetrics::get_generate_duration)
        .def("get_tokenization_duration", &PerfMetrics::get_tokenization_duration)
        .def("get_detokenization_duration", &PerfMetrics::get_detokenization_duration)
        .def("get_throughput", &PerfMetrics::get_throughput)
        .def("get_tpot", &PerfMetrics::get_tpot)
        .def("get_ttft", &PerfMetrics::get_ttft)
        .def("get_load_time", &PerfMetrics::get_load_time)
        .def("__add__", &PerfMetrics::operator+)
        .def("__iadd__", &PerfMetrics::operator+=)
        .def_readonly("raw_metrics", &PerfMetrics::raw_metrics);

    py::class_<TokenizedInputs>(m, "TokenizedInputs")
        .def(py::init<ov::Tensor, ov::Tensor>())
        .def_readwrite("input_ids", &TokenizedInputs::input_ids)
        .def_readwrite("attention_mask", &TokenizedInputs::attention_mask);

    py::class_<EncodedResults>(m, "EncodedResults", encoded_results_docstring)
        .def_readonly("tokens", &EncodedResults::tokens)
        .def_readonly("scores", &EncodedResults::scores)
        .def_readonly("perf_metrics", &EncodedResults::perf_metrics);

    py::class_<StreamerBase, ConstructableStreamer, std::shared_ptr<StreamerBase>>(m, "StreamerBase", streamer_base_docstring)  // Change the holder form unique_ptr to shared_ptr
        .def(py::init<>())
        .def("put", &StreamerBase::put, "Put is called every time new token is decoded. Returns a bool flag to indicate whether generation should be stoped, if return true generation stops")
        .def("end", &StreamerBase::end, "End is called at the end of generation. It can be used to flush cache if your own streamer has one");

    py::class_<GenerationResult>(m, "GenerationResult", generation_result_docstring)
        .def(py::init<>())
        .def_readonly("m_request_id", &GenerationResult::m_request_id)
        .def_property("m_generation_ids",
            [](GenerationResult &r) -> py::list {
                py::list res;
                for (auto s: r.m_generation_ids) {
                    PyObject* py_s = PyUnicode_DecodeUTF8(s.data(), s.length(), "replace");
                    res.append(py_s);
                }
                return res;
            },
            [](GenerationResult &r, std::vector<std::string> &generation_ids) {
                r.m_generation_ids = generation_ids;
            })
        .def_readwrite("m_scores", &GenerationResult::m_scores)
        .def("__repr__",
            [](const GenerationResult &r) -> py::str{
                std::stringstream stream;
                stream << "<py_continuous_batching.GenerationResult " << r << ">";
                std::string str = stream.str();
                PyObject* py_s = PyUnicode_DecodeUTF8(str.data(), str.length(), "replace");
                return py::reinterpret_steal<py::str>(py_s);
            }
        )
        .def("get_generation_ids",
        [](GenerationResult &r) -> py::list {
            py::list res;
            for (auto s: r.m_generation_ids) {
                PyObject* py_s = PyUnicode_DecodeUTF8(s.data(), s.length(), "replace");
                res.append(py_s);
            }
            return res;
        });

    py::class_<SchedulerConfig>(m, "SchedulerConfig", scheduler_config_docstring)
        .def(py::init<>())
        .def_readwrite("max_num_batched_tokens", &SchedulerConfig::max_num_batched_tokens)
        .def_readwrite("num_kv_blocks", &SchedulerConfig::num_kv_blocks)
        .def_readwrite("cache_size", &SchedulerConfig::cache_size)
        .def_readwrite("block_size", &SchedulerConfig::block_size)
        .def_readwrite("dynamic_split_fuse", &SchedulerConfig::dynamic_split_fuse)
        .def_readwrite("max_num_seqs", &SchedulerConfig::max_num_seqs)
        .def_readwrite("enable_prefix_caching", &SchedulerConfig::enable_prefix_caching);

    py::class_<ContinuousBatchingPipeline>(m, "ContinuousBatchingPipeline", "This class is used for generation with LLMs with continuous batchig")
        .def(py::init([](const std::string& model_path, const SchedulerConfig& scheduler_config, const std::string& device, const std::map<std::string, py::object>& llm_plugin_config, const std::map<std::string, py::object>& tokenizer_plugin_config) {
            ScopedVar env_manager(ov_tokenizers_module_path());
            return std::make_unique<ContinuousBatchingPipeline>(model_path, scheduler_config, device, properties_to_any_map(llm_plugin_config), properties_to_any_map(tokenizer_plugin_config));
        }), py::arg("model_path"), py::arg("scheduler_config"), py::arg("device") = "CPU", py::arg("llm_plugin_config") = ov::AnyMap({}), py::arg("tokenizer_plugin_config") = ov::AnyMap({}))
        .def(py::init([](const std::string& model_path, const ov::genai::Tokenizer& tokenizer, const SchedulerConfig& scheduler_config, const std::string& device, const std::map<std::string, py::object>& plugin_config) {
            ScopedVar env_manager(ov_tokenizers_module_path());
            return std::make_unique<ContinuousBatchingPipeline>(model_path, tokenizer, scheduler_config, device, properties_to_any_map(plugin_config));
        }), py::arg("model_path"), py::arg("tokenizer"), py::arg("scheduler_config"), py::arg("device") = "CPU", py::arg("plugin_config") = ov::AnyMap({}))
        .def("get_tokenizer", &ContinuousBatchingPipeline::get_tokenizer)
        .def("get_config", &ContinuousBatchingPipeline::get_config)
        .def("add_request", py::overload_cast<uint64_t, const ov::Tensor&, const ov::genai::GenerationConfig&>(&ContinuousBatchingPipeline::add_request))
        .def("add_request", py::overload_cast<uint64_t, const std::string&, const ov::genai::GenerationConfig&>(&ContinuousBatchingPipeline::add_request))
        .def("step", &ContinuousBatchingPipeline::step)
        .def("has_non_finished_requests", &ContinuousBatchingPipeline::has_non_finished_requests)
        .def(
            "generate",
            py::overload_cast<const std::vector<ov::Tensor>&, const std::vector<ov::genai::GenerationConfig>&, const ov::genai::StreamerVariant&>(&ContinuousBatchingPipeline::generate),
            py::arg("input_ids"),
            py::arg("sampling_params"),
            py::arg("streamer") = std::monostate{}
        )
        .def(
            "generate",
            py::overload_cast<const std::vector<std::string>&, const std::vector<ov::genai::GenerationConfig>&, const ov::genai::StreamerVariant&>(&ContinuousBatchingPipeline::generate),
            py::arg("prompts"),
            py::arg("sampling_params"),
            py::arg("streamer") = std::monostate{}
        );
}
