# STRUCTURE.md — VoiceBridge / Lunara

> Карта репозитория. Обновляй когда добавляешь новые файлы или папки.
> Последнее обновление: июнь 2026

---

## Структура репозитория

```
voicebridge-server/
│
├── VoiceBridge_Firmware/    # Прошивка ESP32-S3 (ESP-IDF проект)
│   ├── main/                # Основной код прошивки (.c / .cpp)
│   ├── data/                # Данные: сертификаты, конфиги
│   ├── tools/               # Вспомогательные скрипты
│   ├── CMakeLists.txt       # Система сборки ESP-IDF
│   ├── partitions.csv       # Разметка памяти ESP32
│   ├── sdkconfig.defaults   # Настройки ESP-IDF по умолчанию
│   └── README.md
│
├── public/                  # Фронтенд — веб-приложение (клиент)
│   └── ...                  # HTML / JS / CSS
│
├── server.js                # Node.js relay сервер
│                            # WebSocket логика, маршрутизация аудио
│
├── start-server.ps1         # Запуск сервера локально (Windows)
│
├── package.json             # Зависимости Node.js
├── package-lock.json        # Лок-файл
├── .gitignore
│
├── AGENTS.md                # ← Читать ПЕРВЫМ. Устав проекта для агентов
├── CONTEXT.md               # Текущее состояние проекта
├── STRUCTURE.md             # Этот файл — карта репозитория
├── TASKS.md                 # Очередь задач
└── README.md                # Описание для GitHub
```

---

## Где что писать

| Задача | Файл / Папка |
|---|---|
| Логика WebSocket, relay, аудио | `server.js` |
| UI приложения (кнопки, экраны) | `public/` |
| Прошивка ESP32 | `VoiceBridge_Firmware/main/` |
| Новая npm зависимость | `package.json` → `npm install` |
| Запустить сервер локально | `start-server.ps1` |

---

## Чего здесь НЕТ (отложено)

| Компонент | Статус |
|---|---|
| Lunara AI (STT→LLM→TTS) | Горизонт 2, не реализована |
| База данных | Не планируется (stateless архитектура) |
| AEC на ESP32 | Горизонт 2, сейчас AEC на сервере |
