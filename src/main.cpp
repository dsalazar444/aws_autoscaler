#include <aws/core/Aws.h>
#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/monitoring/CloudWatchClient.h>
#include <aws/ec2/EC2Client.h>

#include <iostream>

int main() {
    Aws::SDKOptions options;

    Aws::InitAPI(options);

    std::cout << "AWS SDK inicializado correctamente\n";

    Aws::ShutdownAPI(options);

    return 0;
}