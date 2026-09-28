#pragma once

#include <string>
#include <aws/autoscaling/AutoScalingClient.h>

enum class ActionResult { Success, Failed };

// Implementacion real de IActioner contra la API de Auto Scaling.
//
// IncreaseCapacity: SetDesiredCapacity -- fija el TOTAL deseado; AWS se
//   encarga de lanzar las instancias que falten.
// ReduceCapacity: TerminateInstanceInAutoScalingGroup con
//   ShouldDecrementDesiredCapacity=true -- AWS se encarga de desregistrar la
//   instancia del Load Balancer y esperar su "deregistration delay" (drenaje
//   de conexiones, habilitado por defecto en ALB/NLB) ANTES de terminarla de
//   verdad. Este codigo no orquesta el drenaje, solo pide la terminacion.
class AWSActioner {
public:
    explicit AWSActioner(std::string asgName);

     // Sube la capacidad DESEADA del ASG a `targetCount` instancias totales
    // (no "agrega N", fija el total -- asi es idempotente si se llama de
    // nuevo con el mismo valor mientras algo ya esta lanzandose).
    ActionResult IncreaseCapacity(int targetCount);
    
    // Reduce quitando puntualmente la instancia `instanceIdToRemove` (la de
    // menor carga, ya elegida por Controller).
    ActionResult ReduceCapacity(const std::string& instanceIdToRemove);

private:
    std::string _asgName;
    Aws::AutoScaling::AutoScalingClient _autoScalingClient;
};