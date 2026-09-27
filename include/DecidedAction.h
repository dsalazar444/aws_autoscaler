#pragma once

#include <optional>
#include <string>

// Este struct está en su propio .h y no en Controller, porque es necesitado
// por varios modulos, y si lo dejamos en Controller puede llevar a 
// dependencia circular
namespace scaleAction
{
    
       // Indica estado de acción decidida, en Controller::Decide()
        enum class Action {Mantain, Increment, Decrement};
        struct DecidedAction {
            Action action;
            int targetCount;
            std::optional<std::string> idToDelete;
            std::string justification;
            
        };
    
} // namespace autoscalerAction