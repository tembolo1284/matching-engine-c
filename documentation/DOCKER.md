# Docker Guide - Matching Engine

## Quick Start

### Build the Image

```bash
docker build -t matching-engine .
```

### Run the Server

```bash
# Default: TCP + multicast, dual-processor mode
docker run -p 1234:1234 -p 5000:5000/udp matching-engine

# TCP only (no multicast)
docker run -p 1234:1234 matching-engine --tcp 1234

# Single-processor mode
docker run -p 1234:1234 matching-engine --tcp 1234 --single-processor

# Custom port
docker run -p 8080:8080 matching-engine --tcp 8080
```

### Connect a Client

From your host machine (or another container):

```bash
./build/tcp_client localhost 1234
> buy IBM 100 50
> sell IBM 100 50
> flush
```

---

## Docker Compose (Multi-Container Testing)

Docker Compose makes it easy to run the full system: server, subscribers, and clients.

### Start the Full Environment

```bash
# Start server and subscriber
docker-compose up

# Or run in background
docker-compose up -d
```

### Run an Interactive Client

```bash
docker-compose run client
> buy IBM 100 50
> sell NVDA 200 30
> flush
> quit
```

### Scale Subscribers

```bash
# Run 3 multicast subscribers
docker-compose up --scale subscriber=3
```

### View Logs

```bash
# All services
docker-compose logs -f

# Just the server
docker-compose logs -f server
```

### Stop Everything

```bash
docker-compose down
```

---

## Testing Scenarios

### Scenario 1: Basic Order Flow

```bash
# Terminal 1: Start server
docker run -p 1234:1234 --name me-server matching-engine --tcp 1234

# Terminal 2: Connect client
./build/tcp_client localhost 1234
> N,1,IBM,100,50,B,1
> N,1,IBM,100,50,S,2
> F
```

Expected output:
- Ack for buy order
- Ack for sell order  
- Trade execution (both orders match at price 100)
- Top-of-book updates

### Scenario 2: Multicast Market Data

```bash
# Terminal 1: Start server with multicast
docker run -p 1234:1234 -p 5000:5000/udp --name me-server matching-engine

# Terminal 2: Start subscriber (inside Docker network)
docker run --network container:me-server matching-engine \
    ./multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders
./build/tcp_client localhost 1234
> buy IBM 100 50
```

The subscriber should receive all acks, trades, and top-of-book updates.

### Scenario 3: Multiple Clients

```bash
# Terminal 1: Server
docker-compose up server

# Terminal 2: Client 1
docker-compose run client
> buy IBM 100 50

# Terminal 3: Client 2
docker-compose run client
> sell IBM 100 50
```

Both clients receive the trade notification.

### Scenario 4: Dual-Processor Symbol Routing

```bash
# Start server (dual-processor is default)
docker run -p 1234:1234 matching-engine

# Connect and send orders to different processors
./build/tcp_client localhost 1234
> buy AAPL 150 100    # → Processor 0 (A-M)
> buy IBM 100 50      # → Processor 0 (A-M)
> buy NVDA 200 75     # → Processor 1 (N-Z)
> buy TSLA 180 60     # → Processor 1 (N-Z)
> flush
```

Check server logs to verify symbol routing.

---

## Network Modes

### Host Network (Linux Only)

For best multicast support on Linux:

```bash
docker run --network host matching-engine --tcp 1234 --multicast 239.255.0.1:5000
```

This shares the host's network stack directly.

### Bridge Network (Default)

```bash
docker run -p 1234:1234 -p 5000:5000/udp matching-engine
```

TCP works fine. Multicast works within Docker but may not reach external subscribers.

### Custom Network

```bash
# Create network
docker network create --subnet 172.28.0.0/16 trading-net

# Run server
docker run --network trading-net --ip 172.28.0.10 -p 1234:1234 matching-engine

# Run subscriber on same network
docker run --network trading-net matching-engine ./multicast_subscriber 239.255.0.1 5000
```

---

## Performance Testing

### Run with Resource Limits

```bash
# Limit to 2 CPUs and 512MB RAM
docker run --cpus 2 --memory 512m -p 1234:1234 matching-engine
```

### Benchmark with Multiple Clients

```bash
# Start server
docker run -p 1234:1234 matching-engine --tcp 1234

# Run load test (from host)
for i in {1..10}; do
    ./build/tcp_client localhost 1234 2 &
done
wait
```

---

## Debugging

### View Container Logs

```bash
docker logs matching-engine-server
docker logs -f matching-engine-server  # Follow
```

### Shell into Running Container

```bash
docker exec -it matching-engine-server /bin/bash
```

### Check Resource Usage

```bash
docker stats matching-engine-server
```

### Inspect Network

```bash
# Check container IP
docker inspect -f '{{range.NetworkSettings.Networks}}{{.IPAddress}}{{end}}' matching-engine-server

# Check port mappings
docker port matching-engine-server
```

---

## Troubleshooting

### "Connection refused" when connecting client

- Check server is running: `docker ps`
- Check port mapping: `docker port matching-engine-server`
- Ensure you're connecting to the right port

### Multicast subscriber not receiving messages

1. Check both containers are on the same Docker network
2. Try `--network host` on Linux
3. Verify multicast is enabled: server should print "Multicast enabled: 239.255.0.1:5000"

### Build fails

- Ensure all source files are present
- Check `.dockerignore` isn't excluding needed files
- Try building without cache: `docker build --no-cache -t matching-engine .`

### Container exits immediately

Check logs for errors:
```bash
docker logs matching-engine-server
```

Common causes:
- Port already in use
- Missing command-line arguments

---

## AWS Deployment

### Push to ECR

```bash
# Authenticate
aws ecr get-login-password --region us-east-1 | \
    docker login --username AWS --password-stdin <account-id>.dkr.ecr.us-east-1.amazonaws.com

# Tag
docker tag matching-engine:latest <account-id>.dkr.ecr.us-east-1.amazonaws.com/matching-engine:latest

# Push
docker push <account-id>.dkr.ecr.us-east-1.amazonaws.com/matching-engine:latest
```

### Run on EC2

```bash
ssh ec2-user@<ec2-ip>

# Install Docker
sudo yum install -y docker
sudo systemctl start docker

# Pull and run
sudo docker pull <account-id>.dkr.ecr.us-east-1.amazonaws.com/matching-engine:latest
sudo docker run -d -p 1234:1234 --name matching-engine \
    <account-id>.dkr.ecr.us-east-1.amazonaws.com/matching-engine:latest
```

### Connect from Anywhere

```bash
./build/tcp_client <ec2-public-ip> 1234
> buy IBM 100 50
```

---

## Command Reference

| Command | Description |
|---------|-------------|
| `docker build -t matching-engine .` | Build the image |
| `docker run -p 1234:1234 matching-engine` | Run server |
| `docker run -p 1234:1234 matching-engine --tcp 1234 --single-processor` | Single-processor mode |
| `docker-compose up` | Start full environment |
| `docker-compose run client` | Interactive client |
| `docker-compose down` | Stop everything |
| `docker logs -f <container>` | View logs |
| `docker exec -it <container> /bin/bash` | Shell access |
