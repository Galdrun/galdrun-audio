#include "app/InputSettings.h"

#include "matp/Session.h"
#include "util/Logger.h"

namespace multiroom {

void currentInputControls(const ClientRuntime& runtime, std::vector<matp::InputControl>& out)
{
    out.clear();
    for (const InputDefinition& input : runtime.config->inputs)
        out.push_back(input.control);
}

InputBindingChange changeInputBindings(ClientRuntime& runtime, const std::string& inputId,
                                       const json::Value& bindings, std::string& error)
{
    ClientConfig& config = *runtime.config;
    InputDefinition* input = nullptr;
    for (InputDefinition& candidate : config.inputs)
    {
        if (candidate.control.id == inputId)
            input = &candidate;
    }
    if (input == nullptr)
    {
        error = "no input \"" + inputId + "\"";
        return InputBindingChange::UnknownInput;
    }

    matp::InputBindings merged = input->control.bindings;
    if (!matp::inputBindingsFromJson(bindings, input->control.kind, merged, error))
        return InputBindingChange::Invalid;
    if (merged == input->control.bindings)
        return InputBindingChange::Unchanged;

    const matp::InputBindings previous = input->control.bindings;
    input->control.bindings = merged;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        input->control.bindings = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return InputBindingChange::NotSaved;
    }

    MULTIROOM_INFO("bindings of input %s changed: %s", inputId.c_str(),
             json::serialize(matp::inputBindingsToJson(input->control.kind, merged)).c_str());
    if (runtime.session != nullptr)
        runtime.session->inputControlsChanged();
    return InputBindingChange::Changed;
}

} // namespace multiroom
