#include "AWSActioner.h"

#include <thread>

#include <aws/autoscaling/model/SetDesiredCapacityRequest.h>
#include <aws/autoscaling/model/TerminateInstanceInAutoScalingGroupRequest.h>

namespace {
constexpr int MAX_RETRIES = 2;  // reintentos ADICIONALES tras el primer intento
constexpr std::chrono::milliseconds RETRY_BACKOFF{200};

// Mismo patron de reintentos que RetryOnFailure en AWSMetricsSource.cpp,
// pero para acciones (que devuelven ActionResult en vez de FetchResult<T>).
// SetDesiredCapacity es naturalmente idempotente (fijar el mismo total dos
// veces no hace daño), asi que reintentarlo a ciegas es seguro. Terminar la
// MISMA instancia dos veces es mas delicado -- ver el comentario dentro de
// ReduceCapacity.

template <typename Fn>
ActionResult RetryAction(Fn&& attempt) {
    for (int tries = 0; tries <= MAX_RETRIES; ++tries) {
        if (attempt() == ActionResult::Success) {
            return ActionResult::Success;
        }
        if (tries < MAX_RETRIES) {
            std::this_thread::sleep_for(RETRY_BACKOFF * (tries + 1));
        }
    }
    return ActionResult::Failed;
}
}  // namespace

AWSActioner::AWSActioner(std::string asgName) : _asgName(std::move(asgName)) {}

// NOTA: Api retorna respuesta cuando inicia proceso, si es success, así que
// eso no significa que la EC2 ya esté terminada o la nueva ya este creada,
// AWS simplemente aceptó/inició la actividad de terminación/creación.
// La documentación muestra que la actividad puede quedar con StatusCode = InProgress

ActionResult AWSActioner::IncreaseCapacity(int targetCount) {

    return RetryAction([this, targetCount]() -> ActionResult {
        Aws::AutoScaling::Model::SetDesiredCapacityRequest request;
        request.SetAutoScalingGroupName(_asgName);
        request.SetDesiredCapacity(targetCount);

        // enviamos request a AWS, usando cliente ASG
        auto outcome = _autoScalingClient.SetDesiredCapacity(request);
        return outcome.IsSuccess() ? ActionResult::Success : ActionResult::Failed;
        // TODO: registrar outcome.GetError() en el log de auditoria si falla
    });
}

ActionResult AWSActioner::ReduceCapacity(const std::string& instanceIdToRemove) {
    return RetryAction([this, &instanceIdToRemove]() -> ActionResult {
        
        Aws::AutoScaling::Model::TerminateInstanceInAutoScalingGroupRequest request;
        request.SetInstanceId(instanceIdToRemove);

        // true: ademas de terminarla, reduce en 1 la capacidad deseada (que es 
        // diferente a min y max instancias en ASG), para
        // que el ASG no la reemplace con una nueva. Si esto dejara al ASG por
        // debajo de su MinSize configurado, AWS rechaza la llamada -- una
        // capa de seguridad adicional.
        request.SetShouldDecrementDesiredCapacity(true);

        // Realizamos peticion
        auto outcome = _autoScalingClient.TerminateInstanceInAutoScalingGroup(request);
        if (outcome.IsSuccess()) {
            return ActionResult::Success;
        }

        // LIMITACIÓN (no resuelto aqui): si el primer intento
        // realmente termino la instancia pero la RESPUESTA se perdio por red,
        // este reintento volveria a pedir terminar una instancia que ya no
        // existe, y probablemente falle con un error. Como no hay un error de
        // "instancia no encontrada", necesitariamos acciones adicionales
        // como consultar nuevamente ASG. En esta version, no se aplicará
        // y esto será una limitación a tener en cuenta.
       
        return ActionResult::Failed;
    });
}