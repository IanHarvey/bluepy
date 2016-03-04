#!/usr/bin/python

import requests
import os,tempfile,errno
from bs4 import BeautifulSoup

def get_html(url, local_filename):

    cachedir = os.path.join(tempfile.gettempdir(),'bluepy')
    try:
        os.mkdir(cachedir)
    except OSError as error:
        if error.errno != errno.EEXIST:
            raise

    cachefilename = os.path.join(cachedir,local_filename)

    try:
        html = file(cachefilename).read()
    except:
        html = requests.get(url).content
        file(cachefilename,'w').write(html)
    return html

def get_table_rows(html=None):
    if html is None:
        html = get_html()

    soup = BeautifulSoup(html)

    tables = soup.find_all("table")
    
    biggest_table = max(tables, key=len)

    #service_table=soup.find("table", attrs={"summary":"Documents This library contains Services."})

    assert(biggest_table)

    for row in biggest_table.find_all('tr'):
        cols = row.find_all('td')
        cols = [ele.text.strip() for ele in cols]
        outrow=[ele for ele in cols if ele] # Get rid of empty values
        if outrow:
            yield outrow

def get_table(url, local_filename, table_defs):
    """ Grabs the largest table from a webpage.
    
    table_defs is a list of column name, interpretation function.
    """
    html = get_html(url, local_filename)
    for row in get_table_rows(html):
        assert(len(row)==len(table_defs))

        ret = {}
        for col,(name,func) in zip(row, table_defs):
            try:
                if func is None:
                    func = lambda x:x
                ret[name] = func(col)
            except:
                print(name)
                print(col)
                print(row)
                raise
        yield ret

def get_service_names():
    for row in get_table('https://developer.bluetooth.org/gatt/services/Pages/ServicesHome.aspx','services.html',
                         (('Name',None),
                          ('Type',None),
                          ('Number',lambda x:int(x,16)),
                          ('Level',None))):
        row['cname'] = row['Type'].split('.')[-1]
        yield row

def get_descriptors():
    for row in get_table('https://developer.bluetooth.org/gatt/descriptors/Pages/DescriptorsHomePage.aspx','descriptors.html',
                         (('Name',None),
                          ('Type',None),
                          ('Number',lambda x:int(x,16)),
                          ('Level',None))):
        row['cname'] = row['Type'].split('.')[-1]
        yield row

def get_characteristics():
    for row in get_table('https://developer.bluetooth.org/gatt/characteristics/Pages/CharacteristicsHome.aspx','characteristics.html',
                         (('Name',None),
                          ('Type',None),
                          ('Number',lambda x:int(x,16)),
                          ('Level',None))):
        row['cname'] = row['Type'].split('.')[-1]
        yield row

def get_units():
    for row in get_table('https://developer.bluetooth.org/gatt/units/Pages/default.aspx','units.html',
                         (('Number',lambda x:int(x,16)),
                          ('Name',None),
                          ('Type',None))):
        row['cname'] = row['Type'].split('.')[-1]
        yield row

def get_formats():
    for row in get_table('https://developer.bluetooth.org/gatt/Pages/FormatTypes.aspx','formats.html',
                         (('Name',None),
                          ('Description',None))):
        row['cname'] = row['Name']
        yield row
 
class Definitions(object):
    def __init__(self):
        self._characteristics = None
        self._units = None
        self._services = None
        self._descriptors = None
        self._formats = None

    @property
    def characteristics(self):
        if not self._characteristics:
            self._characteristics = list(get_characteristics())
        return self._characteristics

    @property
    def units(self):
        if not self._units:
            self._units = list(get_units())
        return self._units
        
    @property
    def services(self):
        if not self._services:
            self._services = list(get_service_names())
        return self._services

    @property
    def descriptors(self):
        if not self._descriptors:
            self._descriptors = list(get_descriptors())
        return self._descriptors

    @property
    def formats(self):
        if not self._formats:
            self._formats = list(get_formats())
        return self._formats

    def data(self):
        """
        Makes tables like this:
        number, name, common name.
        """
        return {'characteristic_UUIDs': 
                [(row['Number'], 
                  row['cname'], 
                  row['Name']) for row in self.characteristics],

                'service_UUIDs':
                [(row['Number'],
                  row['cname'],
                  row['Name']) for row in self.services],
                
                'descriptor_UUIDs':
                [(row['Number'],
                  row['cname'],
                  row['Name']) for row in self.descriptors],
                
                'units_UUIDs':
                [(row['Number'],
                  row['cname'],
                  row['Name']) for row in self.units],
        
                'formats':
                [(row['Name'],
                  row['Description']) for row in self.formats],
              
              }

if __name__=="__main__":
    d=Definitions()
    
    import json
    s = json.dumps(d.data(), indent = 4,
                   encoding='utf-8',
                   ensure_ascii = False,
                   sort_keys = True)

    print(s.encode('utf-8'))

