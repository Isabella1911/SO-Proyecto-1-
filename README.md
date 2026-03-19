# Chat — CC3064 Proyecto 1

Aplicación de chat cliente-servidor en C++ con sockets TCP, multithreading y Protocol Buffers.

## Estructura del proyecto

```
.
├── Makefile
├── README.md
├── protos/
│   ├── common.proto                        # StatusEnum (ACTIVE, DO_NOT_DISTURB, INVISIBLE)
│   ├── cliente-side/                       # Mensajes: Cliente → Servidor
│   │   ├── register.proto                  # Type 1  — Registro de usuario
│   │   ├── message_general.proto           # Type 2  — Mensaje broadcast
│   │   ├── message_dm.proto                # Type 3  — Mensaje directo
│   │   ├── change_status.proto             # Type 4  — Cambio de status
│   │   ├── list_users.proto                # Type 5  — Listar usuarios
│   │   ├── get_user_info.proto             # Type 6  — Info de un usuario
│   │   └── quit.proto                      # Type 7  — Desconexión
│   └── server-side/                        # Mensajes: Servidor → Cliente
│       ├── server_response.proto           # Type 10 — Respuesta general
│       ├── all_users.proto                 # Type 11 — Lista de usuarios
│       ├── for_dm.proto                    # Type 12 — DM reenviado
│       ├── broadcast_messages.proto        # Type 13 — Broadcast reenviado
│       └── get_user_info_response.proto    # Type 14 — Info de usuario
└── src/
    ├── server/
    │   └── server.cpp                      # Servidor completo
    └── client/
        └── client.cpp                      # Cliente completo
```

Después de compilar los protos, se generan archivos adicionales en `src/`:

```
src/
├── *.pb.h          # Headers generados por protoc
├── *.pb.cc         # Implementaciones generadas por protoc
├── server/
│   └── server.cpp
└── client/
    └── client.cpp
```

## Dependencias

- **g++** con soporte para C++17
- **protoc** (Protocol Buffers compiler)
- **libprotobuf-dev** (librería de protobuf para C++)
- **pkg-config**

### Instalación en Ubuntu/Debian

```bash
sudo apt-get install -y g++ protobuf-compiler libprotobuf-dev pkg-config
```

## Compilación

```bash
# Compilar todo (protos + server + client)
make

# O paso a paso:
make protos     # Genera los .pb.h y .pb.cc en src/
make server     # Compila ./server
make client     # Compila ./client

# Limpiar archivos generados y binarios
make clean
```

## Ejecución

### Servidor

```bash
./server <puerto>
```

Ejemplo:

```bash
./server 8080
```

El servidor escucha en todas las interfaces (`0.0.0.0`) en el puerto indicado.

### Cliente

```bash
./client <nombre_usuario> <IP_del_servidor> <puerto>
```

Ejemplos:

```bash
# Conexión local
./client alice 127.0.0.1 8080

# Conexión en red (usar la IP de la máquina del servidor)
./client bob 192.168.1.100 8080
```

## Comandos del cliente

| Comando | Descripción |
|---|---|
| `\broadcast <mensaje>` | Enviar mensaje a todos los usuarios |
| `\dm <usuario> <mensaje>` | Enviar mensaje directo a un usuario |
| `\status <STATUS>` | Cambiar status: `ACTIVE`, `DO_NOT_DISTURB`, `INVISIBLE` |
| `\users` | Listar usuarios conectados y su status |
| `\info <usuario>` | Ver IP y status de un usuario |
| `\help` | Mostrar ayuda |
| `\quit` | Salir del chat |

Escribir texto sin `\` lo envía como broadcast automáticamente.

## Protocolo

Cada mensaje TCP lleva un header de 5 bytes:

```
[1 byte: tipo] [4 bytes: longitud (big-endian)] [N bytes: protobuf payload]
```

| Type | Dirección | Proto |
|------|-----------|-------|
| 1 | cliente → servidor | register |
| 2 | cliente → servidor | message_general |
| 3 | cliente → servidor | message_dm |
| 4 | cliente → servidor | change_status |
| 5 | cliente → servidor | list_users |
| 6 | cliente → servidor | get_user_info |
| 7 | cliente → servidor | quit |
| 10 | servidor → cliente | server_response |
| 11 | servidor → cliente | all_users |
| 12 | servidor → cliente | for_dm |
| 13 | servidor → cliente | broadcast_messages |
| 14 | servidor → cliente | get_user_info_response |

## Características

- **Multithreading:** el servidor crea un thread por cada cliente conectado
- **Concurrencia segura:** acceso a datos compartidos protegido con `std::mutex`
- **Inactividad:** thread dedicado que cambia el status a `INVISIBLE` después de 60 segundos sin actividad (configurable en `server.cpp`)
- **Detección de desconexión:** el servidor detecta si un cliente se desconecta inesperadamente y lo remueve del registro
