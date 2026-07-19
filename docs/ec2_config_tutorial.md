# EC2 UDP Server Configuration

## 1. Open EC2

In the AWS Console, open **EC2** and select **Launch instance**.

## 2. Start a New Instance

From the EC2 dashboard, click **Launch instance**.

<img src="./ec2_config/launch_ec2_painel.png" alt="Launch instance button in the EC2 dashboard" width="900">

## 3. Name and AMI

Fill in:

- **Name**: `ntn-udp-server`
- **Application and OS Images (Amazon Machine Image)**: Ubuntu Server

<img src="./ec2_config/add_name_ec2_update_os.png" alt="EC2 instance name and Ubuntu AMI selection" width="760">

## 4. Create a Key Pair

In **Key pair (login)**, choose **Create new key pair**.

<img src="./ec2_config/create_key_pair.png" alt="Create new key pair option" width="760">

Configure:

- **Key pair name**: `nb-iot-ntn-poc-key`
- **Key pair type**: `RSA`
- **Private key file format**: `.pem`

Then click **Create key pair**.

<img src="./ec2_config/key_pair_config.png" alt="Key pair configuration" width="520">

## 5. Select the Key Pair

Confirm the new key pair is selected in **Key pair name**.

<img src="./ec2_config/select_key_pair.png" alt="Selected EC2 key pair" width="760">

## 6. Network Settings

In **Network settings**, keep the default VPC and enable a public IP.

Create a new security group:

- **Security group name**: `nb-iot-ntn-udp-sg`
- **Description**: `Group NTN`

Add inbound rules:

- **SSH**, TCP, port `22`, source `My IP`
- **Custom UDP**, UDP, port `9000`, source `0.0.0.0/0`

<img src="./ec2_config/network_settings.png" alt="EC2 network settings and inbound security group rules" width="520">

## 7. Launch

Review the configuration and click **Launch instance**.

<img src="./ec2_config/create_ec2_instance_ec2.png" alt="EC2 instance launch in progress" width="900">

Wait for the launch to finish.

<img src="./ec2_config/create_ec2_instance_ec2_success.png" alt="EC2 instance launch success" width="900">

## 8. Confirm the Instance

Go to **Instances** and wait until the instance state is **Running**.

<img src="./ec2_config/ec2_instance_list.png" alt="EC2 instance list showing the running instance" width="900">

Open the instance details and copy the **Public IPv4 address**.

<img src="./ec2_config/instance_info.png" alt="EC2 instance public IPv4 address" width="900">

## 9. Connect with SSH

Use the EC2 **Connect to instance** page and copy the SSH command.

<img src="./ec2_config/ssh_connect.png" alt="EC2 SSH connection command" width="760">

Run the command locally using the downloaded `.pem` file:

```bash
ssh -i "nb-iot-ntn-poc-key.pem" ubuntu@<public-ipv4-address>
```

Successful connection:

<img src="./ec2_config/ubuntu_server_connec_success.png" alt="Successful SSH connection to Ubuntu Server" width="760">
