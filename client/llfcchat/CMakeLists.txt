cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    message(FATAL_ERROR
      "This project requires the \"Ninja Multi-Config\" generator. "
      "Configure with: cmake -G \"Ninja Multi-Config\" ...")
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a separate build directory.")
  endif()
  get_filename_component(_llfc_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(VCPKG_MANIFEST_DIR "${_llfc_repo_root}" CACHE PATH "vcpkg manifest directory" FORCE)
  set(VCPKG_INSTALLED_DIR "${_llfc_repo_root}/vcpkg_installed" CACHE PATH "vcpkg installed directory" FORCE)
  set(LLFC_REPO_ROOT "${_llfc_repo_root}" CACHE PATH "llfcchat repository root" FORCE)

  project(llfcchat LANGUAGES CXX RC)

  list(APPEND CMAKE_MODULE_PATH "${LLFC_REPO_ROOT}/cmake")
  include(Dependencies)

  set(CMAKE_CXX_STANDARD 11)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
else()
  # Aggregated from repo root — ensure RC language for .rc embedding under Ninja.
  enable_language(RC)
  if(NOT DEFINED LLFC_REPO_ROOT)
    get_filename_component(LLFC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(LLFC_REPO_ROOT "${LLFC_REPO_ROOT}" CACHE PATH "llfcchat repository root" FORCE)
  endif()
endif()

llfc_find_client_dependencies()

set(LLFCCHAT_SOURCES
  src/BubbleFrame.cpp
  src/ChatItemBase.cpp
  src/ChatView.cpp
  src/ClickableLabel.cpp
  src/MessageTextEdit.cpp
  src/PictureBubble.cpp
  src/FileBubble.cpp
  src/TextBubble.cpp
  src/adduseritem.cpp
  src/applyfriend.cpp
  src/applyfrienditem.cpp
  src/applyfriendlist.cpp
  src/applyfriendpage.cpp
  src/authenfriend.cpp
  src/chatdialog.cpp
  src/chatpage.cpp
  src/chatsyncmanager.cpp
  src/chatuserlist.cpp
  src/chatuserwid.cpp
  src/clickedbtn.cpp
  src/clickedlabel.cpp
  src/clickedoncelabel.cpp
  src/contactuserlist.cpp
  src/conuseritem.cpp
  src/customizeedit.cpp
  src/customizetextedit.cpp
  src/filetcpmgr.cpp
  src/findfaildlg.cpp
  src/findsuccessdlg.cpp
  src/friendinfopage.cpp
  src/friendlabel.cpp
  src/global.cpp
  src/grouptipitem.cpp
  src/httpmgr.cpp
  src/imagecropperlabel.cpp
  src/invaliditem.cpp
  src/lineitem.cpp
  src/listitembase.cpp
  src/loadingdlg.cpp
  src/localchatdb.cpp
  src/localchatstore.cpp
  src/logindialog.cpp
  src/main.cpp
  src/mainwindow.cpp
  src/outboxdispatcher.cpp
  src/registerdialog.cpp
  src/resetdialog.cpp
  src/searchlist.cpp
  src/statelabel.cpp
  src/statewidget.cpp
  src/tcpmgr.cpp
  src/timerbtn.cpp
  src/userdata.cpp
  src/userinfopage.cpp
  src/usermgr.cpp
)

set(LLFCCHAT_HEADERS
  include/BubbleFrame.h
  include/ChatItemBase.h
  include/ChatView.h
  include/MessageTextEdit.h
  include/ClickableLabel.h
  include/PictureBubble.h
  include/FileBubble.h
  include/TextBubble.h
  include/adduseritem.h
  include/applyfriend.h
  include/applyfrienditem.h
  include/applyfriendlist.h
  include/applyfriendpage.h
  include/authenfriend.h
  include/chatdialog.h
  include/chatpage.h
  include/chatsyncmanager.h
  include/chatuserlist.h
  include/chatuserwid.h
  include/clickedbtn.h
  include/clickedlabel.h
  include/clickedoncelabel.h
  include/contactuserlist.h
  include/conuseritem.h
  include/customizeedit.h
  include/customizetextedit.h
  include/findfaildlg.h
  include/filetcpmgr.h
  include/findsuccessdlg.h
  include/friendinfopage.h
  include/friendlabel.h
  include/global.h
  include/grouptipitem.h
  include/httpmgr.h
  include/imagecropperdialog.h
  include/imagecropperlabel.h
  include/invaliditem.h
  include/lineitem.h
  include/listitembase.h
  include/loadingdlg.h
  include/localchatdb.h
  include/localchatstore.h
  include/localmessageDTO.h
  include/logindialog.h
  include/mainwindow.h
  include/outboxdispatcher.h
  include/registerdialog.h
  include/resetdialog.h
  include/searchlist.h
  include/singleton.h
  include/statelabel.h
  include/statewidget.h
  include/tcpmgr.h
  include/timerbtn.h
  include/userdata.h
  include/userinfopage.h
  include/usermgr.h
)

set(LLFCCHAT_UIS
  ui/adduseritem.ui
  ui/applyfriend.ui
  ui/applyfrienditem.ui
  ui/applyfriendpage.ui
  ui/authenfriend.ui
  ui/chatdialog.ui
  ui/chatpage.ui
  ui/chatuserwid.ui
  ui/conuseritem.ui
  ui/findfaildlg.ui
  ui/findsuccessdlg.ui
  ui/friendinfopage.ui
  ui/friendlabel.ui
  ui/grouptipitem.ui
  ui/lineitem.ui
  ui/loadingdlg.ui
  ui/logindialog.ui
  ui/mainwindow.ui
  ui/registerdialog.ui
  ui/resetdialog.ui
  ui/userinfopage.ui
)

add_executable(llfcchat WIN32
  ${LLFCCHAT_SOURCES}
  ${LLFCCHAT_HEADERS}
  ${LLFCCHAT_UIS}
  resources/rc.qrc
  resources/llfcchat.rc
)

set_target_properties(llfcchat PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
  AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui"
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(llfcchat PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(llfcchat PRIVATE QT_DEPRECATED_WARNINGS)

target_link_libraries(llfcchat PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Network
  Qt5::Sql
  Qt5::Widgets
  Qt5::WinMain
)

# Static plugin import for platforms / imageformats / styles (qt5-base + qt5-imageformats).
qt5_import_plugins(llfcchat
  INCLUDE_BY_TYPE platforms Qt5::QWindowsIntegrationPlugin
  INCLUDE_BY_TYPE imageformats
    Qt5::QGifPlugin
    Qt5::QJpegPlugin
    Qt5::QWebpPlugin
  INCLUDE_BY_TYPE styles Qt5::QWindowsVistaStylePlugin
  INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin
)

llfc_apply_msvc_defaults(llfcchat)
llfc_set_runtime_outdir(llfcchat llfcchat)

add_custom_command(TARGET llfcchat POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/config.ini"
    "$<TARGET_FILE_DIR:llfcchat>/config.ini"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/static"
    "$<TARGET_FILE_DIR:llfcchat>/static"
  COMMENT "Deploy llfcchat config.ini and static assets"
  VERBATIM
)
