#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "item_widget.h"
#include "paged_item_widget.h"

#ifdef QT_DEBUG

#if defined(QT_TESTLIB_LIB) && QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
#include <QAbstractItemModelTester>
#endif

#endif // QT_DEBUG

enum class FilterMode {
  StaticString = 0
  , RegEx
};

/// \note place no more than kItemsPerPage items into each ItemListModel
static int kItemsPerPage = 2;

static int filterColumn = static_cast<int>(ItemModel::Columns::Name);

static int sortColumn = static_cast<int>(ItemModel::Columns::Name);

static int sortRoleItemTableProxyFilterModel = static_cast<int>(ItemTableProxyFilterModel::SortRole::SourceRow);

static int sortRolePagedItemTableProxyFilterModel = static_cast<int>(PagedItemTableProxyFilterModel::SortRole::SourceRow);

static bool isDisconnected = false;

static bool enableAbstractItemModelTester = true;

// TODO: support regex on remote side
static FilterMode filterMode = FilterMode::StaticString;

//static std::shared_ptr<fetchedPageData> lastFetchedData;

struct FilterSettings {
  QRegExp filterRegExp;
  Qt::CaseSensitivity filterCaseSensitivity;
};

static QVector<Item> dummyRemoteItems {
  Item{0, "0Atas", "Bork"},
  Item{1, "1Viktor", "Irman"},
  Item{2, "2Nadya", "Makeyn"},
  Item{3, "3Alli", "Shtadt"},
  Item{4, "4Gabriel", "Abrim"},
  Item{5, "5Gabram", "Bdhim"},
  Item{6, "6Ludovik", "Manstein"},
  Item{7, "7Klark", "Kent"},
  Item{8, "8Kattie", "Klark"},
  Item{9, "9Lisa", "Ali"},
  Item{10, "10Siren", "Ann"},
  Item{11, "11Kennie", "Demer"},
  Item{12, "12Clementine", "Dallas"},
};

static ItemModel* createItemModel(int guid, const QString& name, const QString& surname, QObject* parent = nullptr) {
  ItemModel* itemModel = new ItemModel();
  if(parent) {
    itemModel->setParent(parent);
  }
  itemModel->setName(name);
  itemModel->setSurname(surname);
  itemModel->setGUID(guid);
  /*QObject::connect(itemModel, &ItemModel::dataChanged, [](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles){
    qDebug() << "ItemModel::dataChanged";
  });*/
  return itemModel;
}

/*static std::shared_ptr<ItemModel> createSharedItemModel(int guid, const QString& name, const QString& surname, QObject* parent = nullptr) {
  std::shared_ptr<ItemModel> itemModel = std::make_shared<ItemModel>();
  if(parent) {
    itemModel->setParent(parent);
  }
  itemModel->setName(name);
  itemModel->setSurname(surname);
  itemModel->setGUID(guid);
  return itemModel;
}*/

static std::shared_ptr<ItemMapper> createItemMapper(ItemModel* itemModel) {
  std::shared_ptr<ItemMapper> itemMapper = std::make_shared<ItemMapper>();
  /// \note allows two-way data editing
  itemMapper->setSubmitPolicy(QDataWidgetMapper::AutoSubmit);
  itemMapper->setModel(itemModel);
  itemMapper->toFirst();
  return itemMapper;
}

/*static ItemWidget* createItemWidget(std::shared_ptr<ItemMapper> itemMapper) {
  ItemWidget* itemWidget = new ItemWidget();
  itemWidget->setMapper(itemMapper);
  itemWidget->setMappings();
  return itemWidget;
}*/

static QList<Item> retrieveRemoteFiltered(const FilterSettings& filter) {
  QList<Item> result;
  for (int i = 0; i < dummyRemoteItems.size(); i++) {
    // TODO: regex support
    const auto& item = dummyRemoteItems.at(i);
    QString filterItem = "";

    switch (filterColumn) {
      case static_cast<int>(ItemModel::Columns::Name): {
        filterItem = item.getName();
        break;
      }
      case static_cast<int>(ItemModel::Columns::Surname): {
        filterItem = item.getSurname();
        break;
      }
      case static_cast<int>(ItemModel::Columns::GUID): {
        filterItem = QString::number(item.getGUID());
        break;
      }
    }

    // indexIn attempts to find a match in str from position offset (0 by default).
    // Returns the position of the first match, or -1 if there was no match.
    const bool isFiltered = filter.filterRegExp.pattern().isEmpty() || filter.filterRegExp.indexIn(filterItem) != -1;

    if (!isFiltered) {
      continue;
    }

    result.push_back(item);
  }
  return result;
}

static std::shared_ptr<fetchedPageData> retrieveRemoteItems(int pageNum, int itemsPerPage, const FilterSettings& filter) {
  if (pageNum < 0) {
    return nullptr;
  }

  std::shared_ptr<fetchedPageData> result = std::make_shared<fetchedPageData>();

  QList<Item> dummyRemotePage;

  auto filtered = retrieveRemoteFiltered(filter);

  int cursorI = pageNum * itemsPerPage;
  if (cursorI >= filtered.size()) {
    qDebug() << "not enough persons on page " << pageNum;
  }

  const int pageItemsLastCursor = std::min(cursorI + itemsPerPage, filtered.size());
  int sentItems = 0;

  for (; cursorI < pageItemsLastCursor; cursorI++) {
    dummyRemotePage.push_back(filtered.at(cursorI));
    sentItems++;
  }

  int pageCount = 1;
  if (itemsPerPage != 0) {
    std::div_t res = std::div(filtered.size(), itemsPerPage);
    // Fast ceiling of an integer division
    pageCount = res.rem ? (res.quot + 1) : res.quot;
  }

  result->items = dummyRemotePage;
  result->requestedPageNum = pageNum;
  result->totalPages = pageCount;
  result->totalItems = filtered.size(); // TODO
  result->recievedItemsCount = sentItems;
  result->requestedPageSize = itemsPerPage;

  return result;
}

static std::shared_ptr<fetchedPageData> fetchRemoteItemsToModel(bool forceClearCache, std::shared_ptr<ItemListModel> model, int pageNum, int itemsPerPage, const FilterSettings& filter)
{
  if (pageNum < 0) {
    return nullptr;
  }

  std::shared_ptr<fetchedPageData> result = retrieveRemoteItems(pageNum, itemsPerPage, filter);

  if (forceClearCache || result->recievedItemsCount == 0) {
    model->removeAllItems();
  }

  if (pageNum >= result->totalPages) {
    qDebug() << "nothing to show!";
    return result;
  }

  const int pageStartCursor = result->requestedPageNum * result->requestedPageSize;

  // place dummy items if not enough items in cache
  const int remoteRowsTotal = pageStartCursor + result->recievedItemsCount;

  model->setRowsMinSpace(remoteRowsTotal);

  int itemPageCursor = pageStartCursor;

  for ( Item& item : result->items) {
    // replace item by rowNum or add new item
    //const std::shared_ptr<ItemMapper> itemMapper = model->getItemAt(itemPageCursor);

    ItemModel* itemModel = createItemModel(item.getGUID(), item.getName(), item.getSurname());
    if (itemModel) {
      std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
      itemModel->setParent(m_itemMapper.get());
      //model->pushBack(m_itemMapper);
      /// \note reserving space guarantees that item to replace exists
      model->replaceItemAt(itemPageCursor, m_itemMapper);
    }

    itemPageCursor++;
  }

  return result;
}

MainWindow::MainWindow(QWidget *parent) :
QMainWindow(parent),
m_ui(new Ui::MainWindow)
{
  m_ui->setupUi(this);

  m_paginationMapper = std::make_shared<PagedItemMapper>();

  m_filterTableProxyModel = new ItemTableProxyFilterModel();

  // dynamicSortFilter ensures that the model is sorted and filtered whenever
  // the contents of the source model change.
  m_filterTableProxyModel->setDynamicSortFilter(true);
  m_filterTableProxyModel->setFilterKeyColumn(filterColumn);

  m_pagedTableProxyModel = new PagedItemTableProxyFilterModel();

  // dynamicSortFilter ensures that the model is sorted and filtered whenever
  // the contents of the source model change.
  m_pagedTableProxyModel->setDynamicSortFilter(true);

  m_pagedListProxyFilterModel = new PagedItemListProxyFilterModel();

  m_pagedListProxyFilterModel->setPageSize(kItemsPerPage); // limit items on widget page

  m_TableProxyModel = new ItemTableProxyModel();
  m_itemListModelCache = std::make_shared<ItemListModel>();
  m_TableProxyModel->setSourceModel(m_itemListModelCache.get());

  m_ui->prevButton->setEnabled(false);
  m_ui->nextButton->setEnabled(false);
  m_ui->pageSizeSpinBox->setValue(kItemsPerPage);
  m_ui->pageNumSpinBox->setValue(0);
  m_ui->clearCacheOnPagingCheckBox->setChecked(false);

  //m_ui->refreshButton->setEnabled(false);
  m_ui->refreshButton->setEnabled(true);

  {
    ItemModel* itemModel = createItemModel(0, "_0Alie", "Bork");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(1, "_1Bob", "Byorn");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(2, "_2Anna", "Kerman");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(3, "_3Hugo", "Geber");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(4, "_4Borat", "Nagler");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  // must be same as dummy NotLoaded model
  {
    ItemModel* itemModel = createItemModel(std::numeric_limits<int>::min(), "", "");
    itemModel->setItemMode(ItemModel::ItemMode::NotLoaded);
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(11, "_11John", "Black");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  {
    ItemModel* itemModel = createItemModel(12, "_12Sara", "Parker");
    std::shared_ptr<ItemMapper> m_itemMapper = createItemMapper(itemModel);
    itemModel->setParent(m_itemMapper.get());
    //ItemWidget* itemWidget = createItemWidget(m_itemMapper);
    //m_ui->scrollVerticalLayout->addWidget(itemWidget);
    m_itemListModelCache->pushBack(m_itemMapper);
  }

  m_paginationMapper->setModel(m_pagedListProxyFilterModel);
  m_paginationMapper->setSubmitPolicy(QDataWidgetMapper::AutoSubmit);

  m_pagedItemListWidget = new PagedItemWidget;
  m_ui->scrollVerticalLayout->addWidget(m_pagedItemListWidget);

  m_paginationMapper->addMapping(m_pagedItemListWidget, static_cast<int>(PagedItemListProxyFilterModel::Columns::Page), m_pagedItemListWidget->personsPagePropertyName());

  connect(m_ui->prevButton, &QAbstractButton::clicked, [this]() {
    const int prevPageIndex = std::max(0, m_lastMapperPageNum - 1);

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    if (!isDisconnected) {
      std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, prevPageIndex, kItemsPerPage, filterSettings);
      onDataFetched(prevPageIndex, lastFetchedData);
      onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);
    } else {
      //lastFetchedData = nullptr;
      onDataFetched(prevPageIndex, nullptr);
      onRowRangeChanged(prevPageIndex*kItemsPerPage, prevPageIndex*kItemsPerPage+kItemsPerPage);
    }

    // allows dynamic loading while using pagination
    m_paginationMapper->setCurrentIndex(prevPageIndex);
  });

  connect(m_ui->pageSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), [this](int state) {
    Q_UNUSED(this);
    kItemsPerPage = state;
  });

  connect(m_ui->goToPageButton, &QPushButton::clicked, [this]()
  {
    int pageNum = m_ui->pageNumSpinBox->value();
    pageNum = std::max(pageNum, 0);

    m_ui->searchEdit->setText(m_ui->searchEdit->text());

    if (filterMode == FilterMode::StaticString) {
      m_filterTableProxyModel->setFilterFixedString(m_ui->searchEdit->text());
    } else if (filterMode == FilterMode::RegEx) {
      m_filterTableProxyModel->setFilterRegExp(m_ui->searchEdit->text());
    }

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    if (!isDisconnected) {
      std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, pageNum, kItemsPerPage, filterSettings);
      onDataFetched(pageNum, lastFetchedData);
      onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);
    } else {
      //lastFetchedData = nullptr;
      onDataFetched(pageNum, nullptr);
      onRowRangeChanged(pageNum*kItemsPerPage, pageNum*kItemsPerPage+kItemsPerPage);
    }

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

    // allows dynamic loading while using pagination
    m_paginationMapper->setCurrentIndex(pageNum);
  });

  connect(m_ui->checkBox, &QCheckBox::stateChanged, [this](int state) {
    Q_UNUSED(this);

    isDisconnected = state > 0 ? true : false;
  });

  connect(m_ui->filterColComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int val) {
    Q_UNUSED(this);

    Q_ASSERT(filterColumn >= 0 && filterColumn <= (static_cast<int>(ItemModel::Columns::Total) - 1));

    filterColumn = val;

    m_filterTableProxyModel->setFilterKeyColumn(filterColumn);

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();
  });

  connect(m_ui->sortColComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int val) {
    Q_UNUSED(this);

    Q_ASSERT(sortColumn >= 0 && sortColumn <= (static_cast<int>(ItemModel::Columns::Total) - 1));

    sortColumn = val;

    m_ui->tableView_2->sortByColumn(sortColumn, Qt::AscendingOrder);

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();
  });

  connect(m_ui->skipNotLoadedCheckBox, &QCheckBox::stateChanged, [this](int state) {
    Q_UNUSED(this);

    m_filterTableProxyModel->setSkipNotLoaded(state > 0);

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

    //qDebug() << "skipNotLoadedCheckBox = " << state;
    //m_ui->refreshButton->setEnabled(!isDisconnected);
  });

  connect(m_ui->searchButton, &QPushButton::clicked, [this]()
  {
    const int resetPageIndex = 0;

    if (filterMode == FilterMode::StaticString) {
      m_filterTableProxyModel->setFilterFixedString(m_ui->searchEdit->text());
    } else if (filterMode == FilterMode::RegEx) {
      m_filterTableProxyModel->setFilterRegExp(m_ui->searchEdit->text());
    }

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    if (!isDisconnected) {
      std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, resetPageIndex, kItemsPerPage, filterSettings);
      onDataFetched(resetPageIndex, lastFetchedData);
      onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);

    } else {
      //lastFetchedData = nullptr;
      onDataFetched(resetPageIndex, nullptr);
      onRowRangeChanged(resetPageIndex*kItemsPerPage, resetPageIndex*kItemsPerPage+kItemsPerPage);
    }

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

    // allows dynamic loading while using pagination
    m_paginationMapper->setCurrentIndex(resetPageIndex);
  });

  connect(m_ui->refreshButton, &QPushButton::clicked, [this]()
  {
    int refreshPageIndex = m_lastMapperPageNum;
    refreshPageIndex = std::max(refreshPageIndex, 0);

    m_ui->searchEdit->setText(m_ui->searchEdit->text());

    if (filterMode == FilterMode::StaticString) {
      m_filterTableProxyModel->setFilterFixedString(m_ui->searchEdit->text());
    } else if (filterMode == FilterMode::RegEx) {
      m_filterTableProxyModel->setFilterRegExp(m_ui->searchEdit->text());
    }

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, refreshPageIndex, kItemsPerPage, filterSettings);
    onDataFetched(refreshPageIndex, lastFetchedData);
    onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);

    // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

     // allows dynamic loading while using pagination
     m_paginationMapper->setCurrentIndex(refreshPageIndex);
  });

  connect(m_ui->resetButton, &QPushButton::clicked, [this]()
  {
    const int resetPageIndex = 0;

    m_ui->searchEdit->setText("");
    m_filterTableProxyModel->setFilterFixedString("");
    //m_filterItemTableProxyModel->setFilterRegExp("");

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    if (!isDisconnected) {
      std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, resetPageIndex, kItemsPerPage, filterSettings);
      onDataFetched(resetPageIndex, lastFetchedData);
      onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);
    } else {
      //lastFetchedData = nullptr;
      onDataFetched(resetPageIndex, nullptr);
      onRowRangeChanged(resetPageIndex*kItemsPerPage, resetPageIndex*kItemsPerPage+kItemsPerPage);
    }

    // // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

    // allows dynamic loading while using pagination
    m_paginationMapper->setCurrentIndex(resetPageIndex);
  });

  connect(m_ui->nextButton, &QAbstractButton::clicked, [this]() {
    const int nextPageIndex = std::max(0, m_lastMapperPageNum + 1);

    FilterSettings filterSettings;
    filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
    filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

    if (!isDisconnected) {
      std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, nextPageIndex, kItemsPerPage, filterSettings);
      onDataFetched(nextPageIndex, lastFetchedData);
      onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);
     } else {
      //lastFetchedData = nullptr;
      onDataFetched(nextPageIndex, nullptr);
      onRowRangeChanged(nextPageIndex*kItemsPerPage, nextPageIndex*kItemsPerPage+kItemsPerPage);
    }

    // allows dynamic loading while using pagination
    m_paginationMapper->setCurrentIndex(nextPageIndex);
  });

  connect(m_paginationMapper.get(), &PagedItemMapper::currentIndexChanged, this, &MainWindow::onMapperIndexChanged);

  m_filterTableProxyModel->setSourceModel(m_TableProxyModel);

  // dynamicSortFilter ensures that the model is sorted and filtered whenever
  // the contents of the source model change.
  m_filterTableProxyModel->setDynamicSortFilter(false);

  m_filterTableProxyModel->setSortRole(sortRoleItemTableProxyFilterModel);
  m_filterTableProxyModel->setSortCaseSensitivity (Qt::CaseInsensitive);
  m_filterTableProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
  // Invalidates the current sorting and filtering.
  m_filterTableProxyModel->invalidate();

  m_pagedTableProxyModel->setSourceModel(m_filterTableProxyModel);

  // dynamicSortFilter ensures that the model is sorted and filtered whenever
  // the contents of the source model change.
  m_pagedTableProxyModel->setDynamicSortFilter(false);

  m_filterTableProxyModel->setSortRole(sortRolePagedItemTableProxyFilterModel);
  m_pagedTableProxyModel->setFilterMinSourceRowIndex(0);
  m_pagedTableProxyModel->setFilterMaxSourceRowIndex(kItemsPerPage);
  // Invalidates the current sorting and filtering.
  m_pagedTableProxyModel->invalidate();

  m_pagedListProxyFilterModel->setSourceModel(m_pagedTableProxyModel);//m_filterItemTableProxyModel);
  m_pagedListProxyFilterModel->setExtraDataSource(m_itemListModelCache.get());

  m_ui->tableView->setModel(m_pagedTableProxyModel);
  m_ui->tableView->setAlternatingRowColors(true);
  m_ui->tableView->setSortingEnabled(true);
  m_ui->tableView->sortByColumn(sortColumn, Qt::AscendingOrder);
  m_ui->tableView->setColumnWidth(0, 150); // name
  m_ui->tableView->setColumnWidth(1, 150); // surname
  m_ui->tableView->update();
  m_ui->tableView->show();

  m_ui->tableView_2->setModel(m_filterTableProxyModel);
  m_ui->tableView_2->setAlternatingRowColors(true);
  m_ui->tableView_2->setSortingEnabled(true);
  m_ui->tableView_2->sortByColumn(sortColumn, Qt::AscendingOrder);
  m_ui->tableView_2->setColumnWidth(0, 150); // name
  m_ui->tableView_2->setColumnWidth(1, 150); // surname
  m_ui->tableView_2->update();
  m_ui->tableView_2->show();

  m_ui->listView->setModel(m_TableProxyModel);
  m_ui->listView->setModelColumn(0);
  m_ui->listView->update();
  m_ui->listView->show();

  m_ui->listView_2->setModel(m_TableProxyModel);
  m_ui->listView_2->setModelColumn(1);
  m_ui->listView_2->update();
  m_ui->listView_2->show();

  m_ui->listView_3->setModel(m_TableProxyModel);
  m_ui->listView_3->setModelColumn(2);
  m_ui->listView_3->update();
  m_ui->listView_3->show();

  if(!isDisconnected) {
    m_fetchDelayTimer = new QTimer;
    m_fetchDelayTimer->setSingleShot(true);
    // imitate remote load delay
    m_fetchDelayTimer->start(1000);
    QObject::connect(m_fetchDelayTimer, &QTimer::timeout, [this](){
        FilterSettings filterSettings;
        filterSettings.filterRegExp = m_filterTableProxyModel->filterRegExp();
        filterSettings.filterCaseSensitivity = m_filterTableProxyModel->filterCaseSensitivity();

        const int requestPageNum = 3;

        std::shared_ptr<fetchedPageData> lastFetchedData = fetchRemoteItemsToModel(m_ui->clearCacheOnPagingCheckBox->isChecked(), m_itemListModelCache, requestPageNum, kItemsPerPage, filterSettings);
        onDataFetched(lastFetchedData->requestedPageNum, lastFetchedData);
        onRowRangeChanged(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize, lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);

        m_paginationMapper->setCurrentIndex(requestPageNum);
    });
  }

#ifdef QT_DEBUG

#if defined(QT_TESTLIB_LIB) && QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
  if (enableAbstractItemModelTester) {
    new QAbstractItemModelTester(m_itemListModelCache.get(), QAbstractItemModelTester::FailureReportingMode::Fatal, this);
    new QAbstractItemModelTester(m_filterTableProxyModel, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
    new QAbstractItemModelTester(m_pagedTableProxyModel, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
    new QAbstractItemModelTester(m_pagedListProxyFilterModel, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
    new QAbstractItemModelTester(m_TableProxyModel, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
  }
#endif

#endif // QT_DEBUG
}

void MainWindow::onRowRangeChanged(int first, int last) {
  //m_pagedItemTableProxyModel->setFilterMinSourceRowIndex(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize);
  //m_pagedItemTableProxyModel->setFilterMaxSourceRowIndex(lastFetchedData->requestedPageNum*lastFetchedData->requestedPageSize+lastFetchedData->requestedPageSize);
  m_pagedTableProxyModel->setFilterMinSourceRowIndex(first);
  m_pagedTableProxyModel->setFilterMaxSourceRowIndex(last);
}

void MainWindow::onDataFetched(int requestedPageNum, std::shared_ptr<fetchedPageData> data) {
  int pagesTotal = 1;
  int pageNum = requestedPageNum;

  if (data) {
    pagesTotal = data->totalPages;

    m_pagedListProxyFilterModel->setPagesTotal(pagesTotal);
    m_pagedListProxyFilterModel->setPageSize(data->requestedPageSize);

    if (data->totalPages <= 0) {
      m_ui->prevButton->setEnabled(false);
      m_ui->nextButton->setEnabled(false);
    }
  } else { // can`t load remote data, go into offline mode
    m_pagedListProxyFilterModel->setPagesTotal(-1);
    m_pagedListProxyFilterModel->setPageSize(kItemsPerPage);

    // Invalidates the current sorting and filtering.
    m_filterTableProxyModel->invalidate();
    m_pagedTableProxyModel->invalidate();

    if (kItemsPerPage != 0) {
      std::div_t res = std::div(m_filterTableProxyModel->rowCount(), kItemsPerPage);
      // Fast ceiling of an integer division
      pagesTotal = res.rem ? (res.quot + 1) : res.quot;
    }

    m_pagedListProxyFilterModel->setPagesTotal(pagesTotal);
  }

  if (pagesTotal <= 0) {
    m_ui->prevButton->setEnabled(false);
    m_ui->nextButton->setEnabled(false);
  } else {
    m_ui->prevButton->setEnabled(pageNum > 0);
    m_ui->nextButton->setEnabled(pageNum < (pagesTotal - 1));
  }

  if (m_paginationMapper->model()->rowCount() <= 0) {
    /// \note currentIndexChanged don`t work on mapper with zero pages
    m_pagedItemListWidget->clearContents();
  }
}

void MainWindow::onMapperIndexChanged(int pageNum) {
  m_lastMapperPageNum = pageNum;

  m_ui->pageNumSpinBox->setValue(pageNum);


  // Invalidates the current sorting and filtering.
  m_filterTableProxyModel->invalidate();
  m_pagedTableProxyModel->invalidate();
}

MainWindow::~MainWindow()
{
  delete m_ui;
}
